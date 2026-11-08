#include "../core/remux_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#define open _open
#define close _close
#endif

namespace muxnexus {
void LogError(const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ERROR: ", tag); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}
void LogInfo(const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stdout, "[%s] ", tag); vfprintf(stdout, fmt, ap); fprintf(stdout, "\n");
    va_end(ap);
}
} // namespace muxnexus

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

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input> <output> <mp4|mov|matroska>\n", argv[0]);
        return 2;
    }
    int in_fd = open(argv[1], O_RDONLY);
    int out_fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in_fd < 0 || out_fd < 0) { perror("open"); return 1; }

    CliCallback cb;
    std::atomic<bool> cancel{false};
    auto result = muxnexus::RunRemux(in_fd, out_fd, argv[3], &cb, &cancel);

    close(in_fd); close(out_fd);
    fprintf(stderr, "\nResult: %s\n", result.message.c_str());
    for (const auto& issue : result.issues) {
        const char* sev = issue.severity == 2 ? "FAIL" : issue.severity == 1 ? "WARN" : "INFO";
        fprintf(stderr, "[%s] %s: %s — %s\n", sev, issue.stream.c_str(),
                issue.message.c_str(), issue.suggestion.c_str());
    }
    return result.state == muxnexus::RemuxState::DONE ? 0 : 1;
}