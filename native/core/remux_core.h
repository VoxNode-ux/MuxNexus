#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace muxnexus {

enum class RemuxState : int {
    IDLE = 0,
    READING_HEADERS = 1,
    REMUXING = 2,
    FINALIZING = 3,
    DONE = 4,
    ERROR = 5,
    CANCELLED = 6
};

struct TelemetrySample {
    int64_t pts_us;
    int64_t dts_us;
    int stream_index;
    int64_t byte_offset;
    int frame_size;
    bool is_keyframe;
};

class RemuxCallback {
public:
    virtual ~RemuxCallback() = default;
    virtual void on_progress(RemuxState state, int percent,
                             const std::string& message) = 0;
    virtual void on_telemetry(const TelemetrySample& sample) = 0;
};

struct RemuxIssue {
    int severity;         // 0=INFO, 1=WARN, 2=FAIL
    std::string stream;
    std::string message;
    std::string suggestion;
};

struct ResumeCheckpoint {
    bool valid = false;
    uint64_t input_hash = 0;
    int64_t output_size = 0;
    std::vector<int64_t> stream_last_dts_us;
};

struct RemuxOptions {
    bool fingerprint_input = true;
    bool check_codec_matrix = true;
    bool emit_content_progress = true;
    bool verify_output = true;
    bool compute_checkpoint = false;

    // How many CONSECUTIVE read errors to tolerate before declaring the input
    // damaged. 500 is right for scratched SD cards and NAS media; 50 (the old
    // default) gave up too early on real-world files. This is a "give up
    // entirely" threshold — single errors never stop the job.
    int64_t max_consecutive_read_errors = 500;

    // Hard wall-clock limit per job. Prevents a hung FFmpeg call from pinning
    // a thread forever. Default 4 h; raise via RemuxOptions for archival jobs.
    int64_t max_job_seconds = 4 * 3600;

    // Custom-AVIO buffer size. 256 KB cuts syscalls 4x vs the old 64 KB,
    // noticeably faster on SAF/HDD/SD. 3 buffers exist (in/out/verify), so
    // 256 KB costs ~768 KB peak RAM — trivial on Android 10+.
    int io_buffer_size = 256 * 1024;

    // Skip the post-write verification pass for very large outputs (default
    // 2 GiB) because it re-reads the header and can cost hundreds of ms.
    // Verification is never skipped for smaller files.
    bool skip_verification_for_large_outputs = true;
    int64_t skip_verification_above_bytes = 2LL * 1024 * 1024 * 1024;
};

struct RemuxResult {
    RemuxState state = RemuxState::ERROR;
    std::string message;
    std::vector<RemuxIssue> issues;
    bool output_verified = false;
    ResumeCheckpoint checkpoint;
};

// Caller retains ownership of both fds and MUST close them after RunRemux
// returns. The engine dup()s internally for verification only.
RemuxResult RunRemux(int input_fd,
                     int output_fd,
                     const std::string& output_format,
                     RemuxCallback* callback,
                     std::atomic<bool>* cancel_flag,
                     const RemuxOptions& options = {});

void LogError(const char* tag, const char* fmt, ...);
void LogInfo(const char* tag, const char* fmt, ...);

} // namespace muxnexus
