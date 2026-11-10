#pragma once
// Stream-level merging: take streams from several input files (for example the
// picture of one video and the audio of another) and write them into a single
// output container WITHOUT re-encoding. Also: inspect an input's streams and
// rank its audio tracks by quality.
//
// Everything here is stream copy. Packets are moved byte-for-byte; only their
// timestamps are shifted and rescaled to the output container's time base.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "remux_core.h"

namespace muxnexus {

enum class StreamType { VIDEO, AUDIO, SUBTITLE, OTHER };

struct StreamInfo {
    int index = -1;                 // stream index inside its file
    StreamType type = StreamType::OTHER;
    std::string codec;              // FFmpeg codec name, e.g. "flac", "aac"
    int channels = 0;               // audio
    int sample_rate = 0;            // audio, Hz
    int bit_depth = 0;              // audio, 0 = unknown
    int64_t bit_rate = 0;           // bits per second, 0 = unknown
    int width = 0;                  // video
    int height = 0;                 // video
    bool lossless = false;          // audio
    bool is_default = false;
    bool is_attached_pic = false;   // video stream that is really cover art
    std::string language;           // "eng", "" when unknown
    std::string title;
    int64_t duration_us = -1;       // -1 = unknown
    int64_t quality_score = 0;      // audio only, higher is better
};

struct ProbeResult {
    bool ok = false;
    std::string error;
    std::string format_name;        // FFmpeg demuxer name, e.g. "matroska,webm"
    int64_t duration_us = -1;
    std::vector<StreamInfo> streams;
};

// Reads the container headers of `fd` (the caller keeps ownership of the fd).
ProbeResult ProbeFd(int fd, int io_buffer_size = 256 * 1024);

// Audio quality heuristic. Lossless beats lossy; among lossless, higher
// bit-depth x sample-rate wins, then more channels; among lossy, higher bit rate
// wins, then more channels. Returns 0 for non-audio streams.
// This ranks what the FILE claims. It can't hear the audio: a lossless wrapper
// around a transcoded lossy source will still rank as lossless.
int64_t AudioQualityScore(const StreamInfo& s);

// Position in `streams` of the highest-scoring audio stream (ties go to the
// earlier one), or -1 when there is none. Pass every stream of the file in
// order and the position equals the stream index.
int PickBestAudio(const std::vector<StreamInfo>& streams);

struct StreamPick {
    enum class Kind {
        VIDEO,       // every video stream (cover art excluded)
        AUDIO,       // every audio stream
        SUBTITLE,    // every subtitle stream
        BEST_AUDIO,  // only the highest quality audio stream
        INDEX        // exactly the stream `index`
    };
    Kind kind = Kind::INDEX;
    int index = -1;
};

struct MergeInput {
    int fd = -1;                    // caller keeps ownership
    // Empty = default: all video + all audio (+ subtitles when writing MKV).
    std::vector<StreamPick> picks;
    // Delay applied to this input's streams. Positive = later, negative =
    // earlier. Use it to line up audio that is slightly out of sync.
    int64_t offset_us = 0;
};

struct MergeOptions {
    // When some inputs supply video and others only audio/subtitles, stop at
    // the end of the video: longer audio is trimmed, shorter audio is reported.
    bool end_with_video = true;
    bool verify_output = true;
    int64_t max_consecutive_read_errors = 500;
    int64_t max_job_seconds = 4 * 3600;
    int io_buffer_size = 256 * 1024;
    int64_t skip_verification_above_bytes = 2LL * 1024 * 1024 * 1024;
};

// Writes the picked streams of all inputs into `output_fd` as `output_format`
// ("matroska", "mp4" or "mov"; default matroska). The caller keeps ownership
// of every fd and must open the output read/write (O_RDWR) so the result can be
// verified. Message strings match RunRemux ("Complete", ...).
RemuxResult RunMerge(const std::vector<MergeInput>& inputs,
                     int output_fd,
                     const std::string& output_format,
                     RemuxCallback* callback,
                     std::atomic<bool>* cancel_flag,
                     const MergeOptions& options = {});

} // namespace muxnexus