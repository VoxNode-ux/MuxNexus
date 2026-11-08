#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVStream;
struct AVPacket;

namespace muxnexus {

enum class ContainerKind {
    UNKNOWN, MP4, MOV, MKV, WEBM, MPEG_TS, MPEG_PS,
    AVI, FLV, OGG, WAV, RAW_AAC, RAW_MP3, RAW_HEVC, RAW_H264
};

struct Fingerprint {
    ContainerKind kind = ContainerKind::UNKNOWN;
    std::string label;
    std::string major_brand;
    int confidence = 0;
};

Fingerprint FingerprintFd(int fd);

enum class CompatRule { ALLOW, ALLOW_WITH_NOTE, WARN, FAIL };

struct CompatResult {
    CompatRule rule = CompatRule::ALLOW;
    std::string note;
    std::string suggestion;
};

CompatResult CheckCodecContainer(int codec_id, const char* output_format);

struct StreamStats {
    int input_index = -1;
    int output_index = -1;
    int64_t packets_in = 0;
    int64_t packets_out = 0;
    int64_t bytes_in = 0;
    int64_t bytes_out = 0;
    int64_t out_of_order_dts = 0;
    int64_t pts_before_dts = 0;
    int64_t first_dts_us = INT64_MIN;
    int64_t last_dts_us = INT64_MIN;
    int64_t duration_us = 0;
    bool has_bsf = false;
};

struct CheckIssue {
    enum class Severity { INFO = 0, WARN = 1, FAIL = 2 };
    Severity severity = Severity::INFO;
    std::string stream_desc;
    std::string message;
    std::string suggestion;
};

class RemuxChecks {
public:
    void attach(AVFormatContext* in, AVFormatContext* out,
                const std::vector<int>& stream_map,
                const std::vector<bool>& stream_has_bsf,
                const char* output_format);

    void preflight(std::vector<CheckIssue>* issues);

    void on_packet_in (int stream_index, int64_t dts_us, int64_t pts_us,
                       int size, bool keyframe);
    void on_packet_out(int stream_index, int64_t dts_us, int64_t pts_us,
                       int size, bool keyframe);

    int content_progress_us() const;
    int64_t interleave_drift_ema_us() const { return drift_ema_us_; }
    int64_t peak_interleave_drift_us() const { return peak_drift_us_; }

    CheckIssue::Severity check_invariants(std::vector<CheckIssue>* issues) const;

    const std::vector<StreamStats>& stats() const { return stats_; }

private:
    std::vector<StreamStats> stats_;
    std::vector<int> stream_map_;
    int64_t input_duration_us_ = 0;
    static constexpr int64_t EMA_N = 100;
    int64_t drift_ema_us_ = 0;
    int64_t peak_drift_us_ = 0;
};

std::vector<CheckIssue> VerifyOutput(AVFormatContext* write_ctx,
                                     AVFormatContext* read_ctx);

uint64_t Fnv1aFdHeadTail(int fd, int64_t size);

} // namespace muxnexus