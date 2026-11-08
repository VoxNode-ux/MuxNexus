#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "remux_core.h"

namespace fs = std::filesystem;
using namespace muxnexus;

namespace {

std::string fixtures_dir() {
    const char* env = std::getenv("MUXNEXUS_FIXTURES_DIR");
    if (env && *env) return env;
    return "fixtures/generated";
}

bool has_fixture(const std::string& name) {
    return fs::exists(fs::path(fixtures_dir()) / name);
}

std::string require_fixture(const char* name) {
    auto p = fs::path(fixtures_dir()) / name;
    if (!fs::exists(p)) return {};
    return p.string();
}

int open_read(const std::string& path) { return open(path.c_str(), O_RDONLY); }
int open_write(const std::string& path) { return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); }

struct Output {
    std::string path;
    ~Output() { if (!path.empty()) fs::remove(path); }
};

struct RecordingCallback : RemuxCallback {
    RemuxState last_state = RemuxState::IDLE;
    int last_percent = 0;
    std::string last_message;
    int telemetry_count = 0;
    std::atomic<bool>* cancel_flag = nullptr;
    int cancel_after_telemetry = -1;

    void on_progress(RemuxState s, int pct, const std::string& msg) override {
        last_state = s; last_percent = pct; last_message = msg;
    }
    void on_telemetry(const TelemetrySample&) override {
        ++telemetry_count;
        if (cancel_flag && cancel_after_telemetry > 0 && telemetry_count >= cancel_after_telemetry) {
            cancel_flag->store(true);
        }
    }
};

}

TEST_CASE("MP4 -> MKV remux succeeds and verifies") {
    auto in = require_fixture("aac.mp4");
    if (in.empty()) return;
    Output out; out.path = "/tmp/muxnexus_integ_mp4_to_mkv.mkv";
    int in_fd = open_read(in); int out_fd = open_write(out.path);
    REQUIRE(in_fd >= 0); REQUIRE(out_fd >= 0);
    RecordingCallback cb;
    RemuxOptions opts; opts.verify_output = true;
    auto r = RunRemux(in_fd, out_fd, "matroska", &cb, nullptr, opts);
    CHECK(r.state == RemuxState::DONE);
    CHECK(r.output_verified);
    CHECK(fs::file_size(out.path) > 0);
}

TEST_CASE("MP4 -> MP4 roundtrip preserves streams") {
    auto in = require_fixture("aac.mp4");
    if (in.empty()) return;
    Output out; out.path = "/tmp/muxnexus_integ_roundtrip.mp4";
    int in_fd = open_read(in); int out_fd = open_write(out.path);
    REQUIRE(in_fd >= 0); REQUIRE(out_fd >= 0);
    RecordingCallback cb;
    auto r = RunRemux(in_fd, out_fd, "mp4", &cb, nullptr);
    CHECK(r.state == RemuxState::DONE);
    CHECK(r.output_verified);
    AVFormatContext* ctx = nullptr;
    REQUIRE(avformat_open_input(&ctx, out.path.c_str(), nullptr, nullptr) == 0);
    CHECK(ctx->nb_streams == 2);
    avformat_close_input(&ctx);
}

TEST_CASE("Vorbis MKV -> MP4 fails with a suggestion") {
    auto in = require_fixture("vorbis.mkv");
    if (in.empty()) return;
    Output out; out.path = "/tmp/muxnexus_integ_vorbis_mp4.mp4";
    int in_fd = open_read(in); int out_fd = open_write(out.path);
    REQUIRE(in_fd >= 0); REQUIRE(out_fd >= 0);
    RecordingCallback cb;
    auto r = RunRemux(in_fd, out_fd, "mp4", &cb, nullptr);
    CHECK(r.state == RemuxState::ERROR);
    bool found_suggestion = false;
    for (const auto& i : r.issues) if (i.suggestion.find("Matroska") != std::string::npos) found_suggestion = true;
    CHECK(found_suggestion);
}

TEST_CASE("Vorbis MKV -> MKV succeeds") {
    auto in = require_fixture("vorbis.mkv");
    if (in.empty()) return;
    Output out; out.path = "/tmp/muxnexus_integ_vorbis_mkv.mkv";
    int in_fd = open_read(in); int out_fd = open_write(out.path);
    REQUIRE(in_fd >= 0); REQUIRE(out_fd >= 0);
    RecordingCallback cb;
    auto r = RunRemux(in_fd, out_fd, "matroska", &cb, nullptr);
    CHECK(r.state == RemuxState::DONE);
}

TEST_CASE("Empty file fails cleanly") {
    auto in = require_fixture("empty.mp4");
    if (in.empty()) return;
    Output out; out.path = "/tmp/muxnexus_integ_empty.mp4";
    int in_fd = open_read(in); int out_fd = open_write(out.path);
    REQUIRE(in_fd >= 0); REQUIRE(out_fd >= 0);
    RecordingCallback cb;
    auto r = RunRemux(in_fd, out_fd, "mp4", &cb, nullptr);
    CHECK(r.state == RemuxState::ERROR);
}

TEST_CASE("Cancel returns CANCELLED with a valid checkpoint") {
    auto in = require_fixture("aac.mp4");
    if (in.empty()) return;
    Output out; out.path = "/tmp/muxnexus_integ_cancel.mkv";
    int in_fd = open_read(in); int out_fd = open_write(out.path);
    REQUIRE(in_fd >= 0); REQUIRE(out_fd >= 0);
    std::atomic<bool> cancel{false};
    RecordingCallback cb;
    cb.cancel_flag = &cancel;
    cb.cancel_after_telemetry = 1;
    RemuxOptions opts; opts.compute_checkpoint = true;
    auto r = RunRemux(in_fd, out_fd, "matroska", &cb, &cancel, opts);
    CHECK(r.state == RemuxState::CANCELLED);
    CHECK(r.checkpoint.valid);
    CHECK(r.checkpoint.output_size > 0);
    CHECK(!r.checkpoint.stream_last_dts_us.empty());
}