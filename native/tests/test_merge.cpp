#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "merge_core.h"
#include "remux_core.h"

namespace fs = std::filesystem;
using namespace muxnexus;

namespace {

std::string fixtures_dir() {
    const char* env = std::getenv("MUXNEXUS_FIXTURES_DIR");
    if (env && *env) return env;
    return "fixtures/generated";
}

std::string fixture(const char* name) {
    auto p = fs::path(fixtures_dir()) / name;
    return fs::exists(p) ? p.string() : std::string();
}

int open_read(const std::string& path) { return open(path.c_str(), O_RDONLY); }
// O_RDWR: verification re-reads the output through a dup of this descriptor.
int open_write(const std::string& path) { return open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644); }

struct Output {
    std::string path;
    ~Output() { if (!path.empty()) fs::remove(path); }
};

struct FdGuard {
    int fd;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() { if (fd >= 0) close(fd); }
};

struct NullCallback : RemuxCallback {
    void on_progress(RemuxState, int, const std::string&) override {}
    void on_telemetry(const TelemetrySample&) override {}
};

StreamPick pick(StreamPick::Kind k, int index = -1) {
    StreamPick p;
    p.kind = k;
    p.index = index;
    return p;
}

// What a file really contains: per media type, the codec names, the number of
// packets, and an FNV-1a hash over all packet payload bytes. Hash equality
// between an input stream and an output stream proves a bit-exact copy.
struct Content {
    std::vector<std::string> video_codecs, audio_codecs;
    uint64_t video_hash = 0, audio_hash = 0;
    int64_t video_packets = 0, audio_packets = 0;
    double duration_s = 0;
};

Content read_content(const std::string& path) {
    Content c;
    AVFormatContext* ctx = nullptr;
    REQUIRE(avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) == 0);
    REQUIRE(avformat_find_stream_info(ctx, nullptr) >= 0);
    c.duration_s = ctx->duration > 0 ? ctx->duration / 1e6 : 0;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const AVCodecParameters* p = ctx->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) c.video_codecs.push_back(avcodec_get_name(p->codec_id));
        if (p->codec_type == AVMEDIA_TYPE_AUDIO) c.audio_codecs.push_back(avcodec_get_name(p->codec_id));
    }
    uint64_t vh = 1469598103934665603ULL, ah = 1469598103934665603ULL;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(ctx, pkt) >= 0) {
        const AVCodecParameters* p = ctx->streams[pkt->stream_index]->codecpar;
        uint64_t* h = nullptr;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) { h = &vh; ++c.video_packets; }
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO) { h = &ah; ++c.audio_packets; }
        if (h) for (int i = 0; i < pkt->size; ++i) { *h ^= pkt->data[i]; *h *= 1099511628211ULL; }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    c.video_hash = vh;
    c.audio_hash = ah;
    return c;
}

} // namespace

// ---------------------------------------------------------------- unit tests

static StreamInfo audio(const char* codec, bool lossless, int rate, int depth, int64_t bitrate, int ch = 2) {
    StreamInfo s;
    s.type = StreamType::AUDIO;
    s.codec = codec;
    s.lossless = lossless;
    s.sample_rate = rate;
    s.bit_depth = depth;
    s.bit_rate = bitrate;
    s.channels = ch;
    return s;
}

TEST_CASE("Audio ranking: lossless beats any lossy bit rate") {
    CHECK(AudioQualityScore(audio("flac", true, 44100, 16, 0)) >
          AudioQualityScore(audio("aac", false, 48000, 0, 512000)));
}

TEST_CASE("Audio ranking: among lossless, bit depth x sample rate wins") {
    CHECK(AudioQualityScore(audio("flac", true, 96000, 24, 0)) >
          AudioQualityScore(audio("flac", true, 44100, 16, 0)));
    CHECK(AudioQualityScore(audio("flac", true, 48000, 24, 0)) >
          AudioQualityScore(audio("flac", true, 48000, 16, 0)));
}

TEST_CASE("Audio ranking: among lossy, higher bit rate wins") {
    CHECK(AudioQualityScore(audio("ac3", false, 48000, 0, 192000)) >
          AudioQualityScore(audio("aac", false, 48000, 0, 64000)));
}

TEST_CASE("Audio ranking: non-audio streams score zero") {
    StreamInfo v;
    v.type = StreamType::VIDEO;
    CHECK(AudioQualityScore(v) == 0);
}

TEST_CASE("PickBestAudio: picks the best, ties go to the earlier, ignores video") {
    std::vector<StreamInfo> s;
    StreamInfo v; v.type = StreamType::VIDEO; v.index = 0; s.push_back(v);
    s.push_back(audio("aac", false, 48000, 0, 64000));
    s.push_back(audio("flac", true, 48000, 24, 0));
    s.push_back(audio("flac", true, 48000, 24, 0));
    CHECK(PickBestAudio(s) == 2);
    CHECK(PickBestAudio({}) == -1);
    std::vector<StreamInfo> only_video{v};
    CHECK(PickBestAudio(only_video) == -1);
}

// ------------------------------------------------------- integration tests

TEST_CASE("Probe lists streams and ranks the audio tracks") {
    auto f = fixture("multi_audio.mkv");
    if (f.empty()) return;
    FdGuard fd(open_read(f));
    REQUIRE(fd.fd >= 0);
    ProbeResult pr = ProbeFd(fd.fd);
    REQUIRE(pr.ok);
    int audio_count = 0;
    for (const auto& s : pr.streams) if (s.type == StreamType::AUDIO) ++audio_count;
    CHECK(audio_count == 3);
    const int best = PickBestAudio(pr.streams);
    REQUIRE(best >= 0);
    CHECK(pr.streams[best].codec == "flac");
    CHECK(pr.streams[best].lossless);
}

TEST_CASE("Audio swap: picture of A + audio of B, copied bit for bit") {
    auto a = fixture("merge_video.mp4");
    auto b = fixture("swap_b.mkv");
    if (a.empty() || b.empty()) return;

    FdGuard fa(open_read(a)), fb(open_read(b));
    Output out; out.path = "/tmp/muxnexus_merge_swap.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fa.fd >= 0); REQUIRE(fb.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput ia; ia.fd = fa.fd; ia.picks = {pick(StreamPick::Kind::VIDEO)};
    MergeInput ib; ib.fd = fb.fd; ib.picks = {pick(StreamPick::Kind::AUDIO)};
    NullCallback cb;
    MergeOptions keep_all;
    keep_all.end_with_video = false;    // copy ALL of B's audio, so the hash can match exactly
    RemuxResult r = RunMerge({ia, ib}, fo.fd, "matroska", &cb, nullptr, keep_all);
    CHECK(r.state == RemuxState::DONE);
    CHECK(r.message == "Complete");
    CHECK(r.output_verified);

    Content in_a = read_content(a), in_b = read_content(b), res = read_content(out.path);
    REQUIRE(res.video_codecs.size() == 1);
    REQUIRE(res.audio_codecs.size() == 1);
    CHECK(res.video_codecs[0] == "h264");
    CHECK(res.audio_codecs[0] == "aac");
    CHECK(res.video_hash == in_a.video_hash);     // picture from A, untouched
    CHECK(res.audio_hash == in_b.audio_hash);     // sound from B, untouched
    CHECK(res.video_packets == in_a.video_packets);
    CHECK(res.audio_packets == in_b.audio_packets);
}

TEST_CASE("Default: audio is cut at the end of the video, not before it") {
    auto a = fixture("merge_video.mp4");   // exactly 2.0 s of video
    auto b = fixture("swap_b.mkv");        // ~2.0 s of AAC, a little longer
    if (a.empty() || b.empty()) return;

    FdGuard fa(open_read(a)), fb(open_read(b));
    Output out; out.path = "/tmp/muxnexus_merge_cut.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fa.fd >= 0); REQUIRE(fb.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput ia; ia.fd = fa.fd; ia.picks = {pick(StreamPick::Kind::VIDEO)};
    MergeInput ib; ib.fd = fb.fd; ib.picks = {pick(StreamPick::Kind::AUDIO)};
    NullCallback cb;
    RemuxResult r = RunMerge({ia, ib}, fo.fd, "matroska", &cb, nullptr);   // default options
    CHECK(r.state == RemuxState::DONE);

    Content in_b = read_content(b), res = read_content(out.path);
    CHECK(res.audio_packets <= in_b.audio_packets);          // never more than B has
    CHECK(res.audio_packets >= in_b.audio_packets - 3);      // at most a few packets cut
    CHECK(res.duration_s <= 2.15);                           // and it ends with the video
}

TEST_CASE("Multiple audio tracks: keep only the best one") {
    auto f = fixture("multi_audio.mkv");
    if (f.empty()) return;

    FdGuard fi(open_read(f));
    Output out; out.path = "/tmp/muxnexus_merge_best.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fi.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput in; in.fd = fi.fd;
    in.picks = {pick(StreamPick::Kind::VIDEO), pick(StreamPick::Kind::BEST_AUDIO)};
    NullCallback cb;
    RemuxResult r = RunMerge({in}, fo.fd, "matroska", &cb, nullptr);
    CHECK(r.state == RemuxState::DONE);
    CHECK(r.output_verified);

    Content res = read_content(out.path);
    REQUIRE(res.audio_codecs.size() == 1);
    CHECK(res.audio_codecs[0] == "flac");
    CHECK(res.video_codecs.size() == 1);
}

TEST_CASE("Lossless audio file + video, MKV output") {
    auto v = fixture("merge_video.mp4");
    auto a = fixture("lossless.flac");
    if (v.empty() || a.empty()) return;

    FdGuard fv(open_read(v)), fa(open_read(a));
    Output out; out.path = "/tmp/muxnexus_merge_flac.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fv.fd >= 0); REQUIRE(fa.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput iv; iv.fd = fv.fd;                           // default picks: all of it
    MergeInput ia; ia.fd = fa.fd; ia.picks = {pick(StreamPick::Kind::BEST_AUDIO)};
    NullCallback cb;
    MergeOptions keep_all;
    keep_all.end_with_video = false;    // copy the whole FLAC so the hash can match exactly
    RemuxResult r = RunMerge({iv, ia}, fo.fd, "matroska", &cb, nullptr, keep_all);
    CHECK(r.state == RemuxState::DONE);
    CHECK(r.output_verified);

    Content in_a = read_content(a), res = read_content(out.path);
    REQUIRE(res.audio_codecs.size() == 1);
    CHECK(res.audio_codecs[0] == "flac");
    CHECK(res.audio_hash == in_a.audio_hash);
}

TEST_CASE("Longer audio is trimmed to the video") {
    auto v = fixture("merge_video.mp4");        // 2 s
    auto a = fixture("lossless_long.flac");     // 4 s
    if (v.empty() || a.empty()) return;

    FdGuard fv(open_read(v)), fa(open_read(a));
    Output out; out.path = "/tmp/muxnexus_merge_trim.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fv.fd >= 0); REQUIRE(fa.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput iv; iv.fd = fv.fd;
    MergeInput ia; ia.fd = fa.fd;
    NullCallback cb;
    RemuxResult r = RunMerge({iv, ia}, fo.fd, "matroska", &cb, nullptr);
    CHECK(r.state == RemuxState::DONE);

    Content res = read_content(out.path);
    CHECK(res.duration_s < 2.6);                // not 4 s
    bool told = false;
    for (const auto& i : r.issues) if (i.message.find("trimmed") != std::string::npos) told = true;
    CHECK(told);
}

TEST_CASE("Delay shifts the audio on the timeline") {
    auto v = fixture("merge_video.mp4");
    auto a = fixture("lossless.flac");
    if (v.empty() || a.empty()) return;

    FdGuard fv(open_read(v)), fa(open_read(a));
    Output out; out.path = "/tmp/muxnexus_merge_delay.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fv.fd >= 0); REQUIRE(fa.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput iv; iv.fd = fv.fd;
    MergeInput ia; ia.fd = fa.fd; ia.offset_us = 500000;   // audio starts 0.5 s late
    NullCallback cb;
    RemuxResult r = RunMerge({iv, ia}, fo.fd, "matroska", &cb, nullptr);
    CHECK(r.state == RemuxState::DONE);

    AVFormatContext* ctx = nullptr;
    REQUIRE(avformat_open_input(&ctx, out.path.c_str(), nullptr, nullptr) == 0);
    REQUIRE(avformat_find_stream_info(ctx, nullptr) >= 0);
    double audio_start = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        AVStream* s = ctx->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && s->start_time != AV_NOPTS_VALUE)
            audio_start = s->start_time * av_q2d(s->time_base);
    }
    avformat_close_input(&ctx);
    CHECK(audio_start >= 0.45);
    CHECK(audio_start <= 0.6);
}

TEST_CASE("MP4 output: video of A + AAC of B") {
    auto a = fixture("merge_video.mp4");
    auto b = fixture("swap_b.mkv");
    if (a.empty() || b.empty()) return;

    FdGuard fa(open_read(a)), fb(open_read(b));
    Output out; out.path = "/tmp/muxnexus_merge_swap.mp4";
    FdGuard fo(open_write(out.path));
    REQUIRE(fa.fd >= 0); REQUIRE(fb.fd >= 0); REQUIRE(fo.fd >= 0);

    MergeInput ia; ia.fd = fa.fd; ia.picks = {pick(StreamPick::Kind::VIDEO)};
    MergeInput ib; ib.fd = fb.fd; ib.picks = {pick(StreamPick::Kind::BEST_AUDIO)};
    NullCallback cb;
    RemuxResult r = RunMerge({ia, ib}, fo.fd, "mp4", &cb, nullptr);
    CHECK(r.state == RemuxState::DONE);
    CHECK(r.output_verified);
    Content res = read_content(out.path);
    CHECK(res.video_codecs.size() == 1);
    CHECK(res.audio_codecs.size() == 1);
}

TEST_CASE("Bad requests fail cleanly") {
    auto a = fixture("merge_video.mp4");
    if (a.empty()) return;
    FdGuard fa(open_read(a));
    Output out; out.path = "/tmp/muxnexus_merge_bad.mkv";
    FdGuard fo(open_write(out.path));
    REQUIRE(fa.fd >= 0); REQUIRE(fo.fd >= 0);
    NullCallback cb;

    SUBCASE("no inputs") {
        RemuxResult r = RunMerge({}, fo.fd, "matroska", &cb, nullptr);
        CHECK(r.state == RemuxState::ERROR);
    }
    SUBCASE("stream index out of range") {
        MergeInput in; in.fd = fa.fd; in.picks = {pick(StreamPick::Kind::INDEX, 99)};
        RemuxResult r = RunMerge({in}, fo.fd, "matroska", &cb, nullptr);
        CHECK(r.state == RemuxState::ERROR);
    }
    SUBCASE("picking audio from a file that has none writes nothing") {
        MergeInput in; in.fd = fa.fd; in.picks = {pick(StreamPick::Kind::BEST_AUDIO)};
        RemuxResult r = RunMerge({in}, fo.fd, "matroska", &cb, nullptr);
        CHECK(r.state == RemuxState::ERROR);
    }
}