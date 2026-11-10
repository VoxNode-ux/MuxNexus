#include "../core/merge_core.h"
#include "../core/remux_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define open _open
#define close _close
#endif

class CliCallback : public muxnexus::RemuxCallback {
public:
    void on_progress(muxnexus::RemuxState state, int percent, const std::string& msg) override {
        const char* names[] = {"IDLE","READING_HEADERS","REMUXING","FINALIZING","DONE","ERROR","CANCELLED"};
        fprintf(stderr, "\r[%s] %3d%% %s", names[(int)state], percent, msg.c_str());
        if (state == muxnexus::RemuxState::DONE || state == muxnexus::RemuxState::ERROR ||
            state == muxnexus::RemuxState::CANCELLED) fprintf(stderr, "\n");
    }
    void on_telemetry(const muxnexus::TelemetrySample& t) override {
        fprintf(stderr, "\r  stream %d  pts=%lldus  size=%d  key=%d   ",
                t.stream_index, (long long)t.pts_us, t.frame_size, (int)t.is_keyframe);
    }
};

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s <input> <output> <mp4|mov|matroska>        convert the container\n"
        "  %s probe <file>                               list streams, rank audio\n"
        "  %s merge <output> <mp4|mov|matroska> <input[:sel,...]>...\n"
        "                                                combine streams from several files\n"
        "\n"
        "Selectors (after the last ':' of an input; default = all video + audio):\n"
        "  v          all video streams        a        all audio streams\n"
        "  s          all subtitle streams     best     only the best audio stream\n"
        "  i<N>       exactly stream N         d<ms>    delay this input by N ms (may be negative)\n"
        "\n"
        "Example: take the picture of a.mp4 and the best audio of b.mkv:\n"
        "  %s merge out.mkv matroska a.mp4:v b.mkv:best\n",
        prog, prog, prog, prog);
}

static int print_result(const muxnexus::RemuxResult& result) {
    fprintf(stderr, "\nResult: %s\n", result.message.c_str());
    for (const auto& issue : result.issues) {
        const char* sev = issue.severity == 2 ? "FAIL" : issue.severity == 1 ? "WARN" : "INFO";
        if (issue.suggestion.empty()) {
            fprintf(stderr, "[%s] %s: %s\n", sev, issue.stream.c_str(), issue.message.c_str());
        } else {
            fprintf(stderr, "[%s] %s: %s — %s\n", sev, issue.stream.c_str(),
                    issue.message.c_str(), issue.suggestion.c_str());
        }
    }
    return result.state == muxnexus::RemuxState::DONE ? 0 : 1;
}

static bool all_digits(const std::string& s, size_t from = 0) {
    if (s.size() <= from) return false;
    for (size_t i = from; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// One selector token: v, a, s, best, i<N>, d<ms>.
static bool parse_selector(const std::string& tok, muxnexus::MergeInput* mi) {
    using Kind = muxnexus::StreamPick::Kind;
    auto add = [&](Kind k, int idx) {
        muxnexus::StreamPick p;
        p.kind = k;
        p.index = idx;
        mi->picks.push_back(p);
    };
    if (tok == "v")    { add(Kind::VIDEO, -1); return true; }
    if (tok == "a")    { add(Kind::AUDIO, -1); return true; }
    if (tok == "s")    { add(Kind::SUBTITLE, -1); return true; }
    if (tok == "best") { add(Kind::BEST_AUDIO, -1); return true; }
    if (tok.size() > 1 && tok[0] == 'i' && all_digits(tok, 1)) {
        add(Kind::INDEX, std::atoi(tok.c_str() + 1));
        return true;
    }
    if (tok.size() > 1 && tok[0] == 'd') {
        const size_t from = tok[1] == '-' ? 2 : 1;
        if (!all_digits(tok, from)) return false;
        mi->offset_us = std::atoll(tok.c_str() + 1) * 1000;
        return true;
    }
    return false;
}

// "path" or "path:sel,sel,...". The part after the last ':' counts as selectors
// only if every token is valid, so paths that contain ':' still work.
static void parse_input_arg(const std::string& arg, std::string* path, muxnexus::MergeInput* mi) {
    const size_t colon = arg.rfind(':');
    if (colon != std::string::npos && colon + 1 < arg.size()) {
        muxnexus::MergeInput trial;
        bool ok = true;
        size_t start = colon + 1;
        while (start <= arg.size()) {
            size_t comma = arg.find(',', start);
            if (comma == std::string::npos) comma = arg.size();
            if (!parse_selector(arg.substr(start, comma - start), &trial)) { ok = false; break; }
            start = comma + 1;
        }
        if (ok) {
            *path = arg.substr(0, colon);
            *mi = trial;
            return;
        }
    }
    *path = arg;
}

static int cmd_probe(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    muxnexus::ProbeResult pr = muxnexus::ProbeFd(fd);
    close(fd);
    if (!pr.ok) { fprintf(stderr, "Probe failed: %s\n", pr.error.c_str()); return 1; }

    printf("File: %s\nFormat: %s\n", path, pr.format_name.c_str());
    if (pr.duration_us >= 0) printf("Duration: %.2f s\n", pr.duration_us / 1e6);
    for (const auto& s : pr.streams) {
        const char* type = s.type == muxnexus::StreamType::VIDEO ? "video"
                         : s.type == muxnexus::StreamType::AUDIO ? "audio"
                         : s.type == muxnexus::StreamType::SUBTITLE ? "subtitle" : "other";
        printf("#%d %s %s", s.index, type, s.codec.c_str());
        if (s.type == muxnexus::StreamType::VIDEO) {
            printf(" %dx%d%s", s.width, s.height, s.is_attached_pic ? " (cover art)" : "");
        } else if (s.type == muxnexus::StreamType::AUDIO) {
            printf(" %dch %dHz", s.channels, s.sample_rate);
            if (s.bit_depth > 0) printf(" %dbit", s.bit_depth);
            if (s.bit_rate > 0) printf(" %lldkbps", (long long)(s.bit_rate / 1000));
            printf(" %s score=%lld", s.lossless ? "lossless" : "lossy", (long long)s.quality_score);
        }
        if (!s.language.empty()) printf(" lang=%s", s.language.c_str());
        if (!s.title.empty()) printf(" title=\"%s\"", s.title.c_str());
        printf("\n");
    }
    const int best = muxnexus::PickBestAudio(pr.streams);
    if (best >= 0) printf("Best audio: #%d (%s)\n", pr.streams[best].index, pr.streams[best].codec.c_str());
    return 0;
}

static int cmd_merge(int argc, char** argv) {
    // argv: <prog> merge <output> <format> <input>...
    if (argc < 5) { usage(argv[0]); return 2; }
    const char* out_path = argv[2];
    const char* format = argv[3];

    std::vector<muxnexus::MergeInput> inputs;
    for (int i = 4; i < argc; ++i) {
        std::string path;
        muxnexus::MergeInput mi;
        parse_input_arg(argv[i], &path, &mi);
        mi.fd = open(path.c_str(), O_RDONLY);
        if (mi.fd < 0) {
            fprintf(stderr, "open %s: ", path.c_str());
            perror("");
            for (auto& in : inputs) close(in.fd);
            return 1;
        }
        inputs.push_back(mi);
    }
    int out_fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        perror("open output");
        for (auto& in : inputs) close(in.fd);
        return 1;
    }

    CliCallback cb;
    std::atomic<bool> cancel{false};
    auto result = muxnexus::RunMerge(inputs, out_fd, format, &cb, &cancel);

    for (auto& in : inputs) close(in.fd);
    close(out_fd);
    return print_result(result);
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "probe") == 0) {
        if (argc != 3) { usage(argv[0]); return 2; }
        return cmd_probe(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "merge") == 0) {
        return cmd_merge(argc, argv);
    }
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    int in_fd = open(argv[1], O_RDONLY);
    int out_fd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (in_fd < 0 || out_fd < 0) { perror("open"); return 1; }

    CliCallback cb;
    std::atomic<bool> cancel{false};
    auto result = muxnexus::RunRemux(in_fd, out_fd, argv[3], &cb, &cancel);

    close(in_fd); close(out_fd);
    return print_result(result);
}