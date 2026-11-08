#include "remux_checks.h"

#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace muxnexus {

namespace {

constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
constexpr uint64_t FNV_PRIME  = 0x100000001b3ULL;

uint64_t fnv1a(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

bool starts_with(const uint8_t* p, size_t n, const uint8_t* sig, size_t sig_n) {
    return n >= sig_n && std::memcmp(p, sig, sig_n) == 0;
}

} // namespace

Fingerprint FingerprintFd(int fd) {
    Fingerprint out;
    uint8_t buf[4096];
    ssize_t n;
    do { n = pread(fd, buf, sizeof(buf), 0); } while (n < 0 && errno == EINTR);
    if (n < 12) return out;
    const size_t len = static_cast<size_t>(n);

    static const uint8_t ebml[] = {0x1A, 0x45, 0xDF, 0xA3};
    if (starts_with(buf, len, ebml, 4)) {
        out.kind = ContainerKind::MKV; out.label = "Matroska/WebM"; out.confidence = 95; return out;
    }

    if (len >= 12 && std::memcmp(buf + 4, "ftyp", 4) == 0) {
        char brand[5] = {0};
        std::memcpy(brand, buf + 8, 4);
        out.major_brand = brand;
        if (std::memcmp(brand, "qt  ", 4) == 0) {
            out.kind = ContainerKind::MOV; out.label = "QuickTime (.mov)";
        } else {
            out.kind = ContainerKind::MP4; out.label = "MP4";
        }
        out.confidence = 98; return out;
    }

    if (len > 188 * 3 && buf[0] == 0x47 && buf[188] == 0x47 && buf[376] == 0x47) {
        out.kind = ContainerKind::MPEG_TS; out.label = "MPEG-TS"; out.confidence = 99; return out;
    }
    if (len > 192 * 3 && buf[4] == 0x47 && buf[196] == 0x47 && buf[388] == 0x47) {
        out.kind = ContainerKind::MPEG_TS; out.label = "M2TS"; out.confidence = 95; return out;
    }

    if (len >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 0xBA) {
        out.kind = ContainerKind::MPEG_PS; out.label = "MPEG-PS"; out.confidence = 90; return out;
    }

    if (len >= 4 && buf[0] == 'F' && buf[1] == 'L' && buf[2] == 'V' && buf[3] == 1) {
        out.kind = ContainerKind::FLV; out.label = "FLV"; out.confidence = 95; return out;
    }

    if (len >= 4 && std::memcmp(buf, "OggS", 4) == 0) {
        out.kind = ContainerKind::OGG; out.label = "Ogg"; out.confidence = 95; return out;
    }

    if (len >= 12 && std::memcmp(buf, "RIFF", 4) == 0) {
        if (std::memcmp(buf + 8, "AVI ", 4) == 0) { out.kind = ContainerKind::AVI; out.label = "AVI"; out.confidence = 95; return out; }
        if (std::memcmp(buf + 8, "WAVE", 4) == 0) { out.kind = ContainerKind::WAV; out.label = "WAV"; out.confidence = 95; return out; }
    }

    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xF6) == 0xF0) {
        out.kind = ContainerKind::RAW_AAC; out.label = "ADTS AAC"; out.confidence = 70; return out;
    }
    if (len >= 3 && std::memcmp(buf, "ID3", 3) == 0) {
        out.kind = ContainerKind::RAW_MP3; out.label = "MP3 (ID3)"; out.confidence = 80; return out;
    }

    size_t off = 0;
    if (len >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1) off = 4;
    else if (len >= 3 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1) off = 3;
    if (off && len > off) {
        uint8_t h264_type = buf[off] & 0x1F;
        uint8_t hevc_type = (buf[off] >> 1) & 0x3F;
        if (hevc_type >= 32 && hevc_type <= 34) {
            out.kind = ContainerKind::RAW_HEVC; out.label = "Raw HEVC (Annex B)"; out.confidence = 85; return out;
        }
        if (h264_type == 5 || h264_type == 7 || h264_type == 8) {
            out.kind = ContainerKind::RAW_H264; out.label = "Raw H.264 (Annex B)"; out.confidence = 85; return out;
        }
    }

    return out;
}

CompatResult CheckCodecContainer(int codec_id, const char* output_format) {
    const bool is_mp4 = std::strcmp(output_format, "mp4") == 0;
    const bool is_mov = std::strcmp(output_format, "mov") == 0;
    const bool is_mp4_family = is_mp4 || is_mov;
    const bool is_mkv = std::strcmp(output_format, "matroska") == 0;

    auto allow  = [] { return CompatResult{CompatRule::ALLOW, {}, {}}; };
    auto allowN = [](std::string n) { return CompatResult{CompatRule::ALLOW_WITH_NOTE, std::move(n), {}}; };
    auto warn   = [](std::string n, std::string s) { return CompatResult{CompatRule::WARN, std::move(n), std::move(s)}; };
    auto fail   = [](std::string n, std::string s) { return CompatResult{CompatRule::FAIL, std::move(n), std::move(s)}; };

    if (is_mkv) {
        switch (codec_id) {
            case AV_CODEC_ID_SUBRIP:
            case AV_CODEC_ID_ASS:
            case AV_CODEC_ID_SSA:
                return allowN("Subtitle will be stored as S_TEXT/*; player support varies.");
            default:
                return allow();
        }
    }

    if (!is_mp4_family) return allow();

    switch (codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_MP3:
        case AV_CODEC_ID_AC3:
        case AV_CODEC_ID_EAC3:
            return allow();

        case AV_CODEC_ID_HEVC:
            return allowN("Tagged hvc1; converted via hevc_mp4toannexb.");

        case AV_CODEC_ID_AV1:
            return allowN("AV1 in MP4 uses the av01 box; supported by FFmpeg 6.1+.");

        case AV_CODEC_ID_VP9:
            return allowN("VP9 in MP4 uses the vp09 box; supported by FFmpeg 6.1+.");

        case AV_CODEC_ID_OPUS:
            return allowN("Opus in MP4 requires the dOps box; older players may reject.");

        case AV_CODEC_ID_FLAC:
            return allowN("FLAC in MP4 requires the dfLa box; older players may reject.");

        case AV_CODEC_ID_VORBIS:
            return warn("Vorbis in MP4 is deprecated and many players reject it.",
                        "Use Matroska output instead.");

        case AV_CODEC_ID_DTS:
            return warn("DTS in MP4 has inconsistent player support.",
                        "Use Matroska if DTS playback fails.");

        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S32LE:
            return warn("Uncompressed PCM in MP4 uses ipcm/lpcm boxes; narrow support.",
                        "Use Matroska or WAV output.");

        case AV_CODEC_ID_TRUEHD:
        case AV_CODEC_ID_MLP:
            return fail("TrueHD/MLP cannot be muxed into MP4.", "Use Matroska output.");

        case AV_CODEC_ID_THEORA:
        case AV_CODEC_ID_VP8:
            return fail("Codec is not supported in MP4/MOV.", "Use Matroska output.");

        case AV_CODEC_ID_SUBRIP:
        case AV_CODEC_ID_ASS:
        case AV_CODEC_ID_SSA:
            return fail("Text subtitle formats are not supported in MP4/MOV.",
                        "Use Matroska output, or drop subtitles.");

        default:
            return allow();
    }
}

void RemuxChecks::attach(AVFormatContext* in, AVFormatContext* /* out */,
                         const std::vector<int>& stream_map,
                         const std::vector<bool>& stream_has_bsf,
                         const char* /* output_format */) {
    stream_map_ = stream_map;
    stats_.assign(in->nb_streams, {});

    if (in->duration > 0 && in->duration != AV_NOPTS_VALUE) {
        input_duration_us_ = in->duration;
    } else {
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            AVStream* s = in->streams[i];
            if (s->duration > 0 && s->duration != AV_NOPTS_VALUE) {
                int64_t us = av_rescale_q(s->duration, s->time_base, {1, 1000000});
                if (us > input_duration_us_) input_duration_us_ = us;
            }
        }
    }

    for (unsigned i = 0; i < in->nb_streams; ++i) {
        auto& st = stats_[i];
        st.input_index = (int)i;
        st.output_index = stream_map[i];
        st.has_bsf = (i < stream_has_bsf.size()) ? stream_has_bsf[i] : false;
        AVStream* is = in->streams[i];
        if (is->duration > 0 && is->duration != AV_NOPTS_VALUE) {
            st.duration_us = av_rescale_q(is->duration, is->time_base, {1, 1000000});
        }
    }
}

void RemuxChecks::preflight(std::vector<CheckIssue>* /* issues */) {}

void RemuxChecks::on_packet_in(int idx, int64_t dts_us, int64_t pts_us,
                               int size, bool /* keyframe */) {
    if (idx < 0 || idx >= (int)stats_.size()) return;
    auto& s = stats_[idx];
    s.packets_in++;
    s.bytes_in += size;

    if (s.first_dts_us == INT64_MIN) s.first_dts_us = dts_us;
    if (s.last_dts_us != INT64_MIN && dts_us < s.last_dts_us - 1000) s.out_of_order_dts++;
    if (s.last_dts_us == INT64_MIN || dts_us > s.last_dts_us) s.last_dts_us = dts_us;
    if (pts_us < dts_us) s.pts_before_dts++;
}

void RemuxChecks::on_packet_out(int idx, int64_t /* dts_us */, int64_t /* pts_us */,
                                int size, bool /* keyframe */) {
    if (idx < 0 || idx >= (int)stats_.size()) return;
    auto& s = stats_[idx];
    s.packets_out++;
    s.bytes_out += size;
}

int RemuxChecks::content_progress_us() const {
    if (input_duration_us_ <= 0) return 0;
    int64_t max_dts = INT64_MIN;
    for (const auto& s : stats_) {
        if (s.last_dts_us > max_dts) max_dts = s.last_dts_us;
    }
    if (max_dts == INT64_MIN || max_dts <= 0) return 0;
    int64_t pct = (max_dts * 100) / input_duration_us_;
    return pct > 99 ? 99 : (int)pct;
}

CheckIssue::Severity RemuxChecks::check_invariants(std::vector<CheckIssue>* issues) const {
    CheckIssue::Severity worst = CheckIssue::Severity::INFO;
    auto raise = [&](CheckIssue::Severity sev) {
        if ((int)sev > (int)worst) worst = sev;
    };

    for (const auto& s : stats_) {
        if (s.output_index < 0) continue;
        std::string desc = "stream#" + std::to_string(s.input_index);
        if (s.packets_in == 0) {
            if (issues) issues->push_back({CheckIssue::Severity::WARN, desc,
                                           "No packets read from this stream.",
                                           "The stream may be empty or the demuxer rejected it."});
            raise(CheckIssue::Severity::WARN);
            continue;
        }
        if (!s.has_bsf && s.bytes_in != s.bytes_out) {
            if (issues) issues->push_back({CheckIssue::Severity::FAIL, desc,
                "Byte-count mismatch on a no-BSF stream: in=" + std::to_string(s.bytes_in) +
                " out=" + std::to_string(s.bytes_out),
                "This should not happen on a remux. Report as a bug."});
            raise(CheckIssue::Severity::FAIL);
        }
        if (s.packets_in != s.packets_out && !s.has_bsf) {
            if (issues) issues->push_back({CheckIssue::Severity::WARN, desc,
                "Packet count differs: in=" + std::to_string(s.packets_in) +
                " out=" + std::to_string(s.packets_out),
                "Some packets may have been dropped."});
            raise(CheckIssue::Severity::WARN);
        }
        if (s.out_of_order_dts > 0) {
            if (issues) issues->push_back({CheckIssue::Severity::WARN, desc,
                std::to_string(s.out_of_order_dts) + " out-of-order DTS values",
                "Player may show A/V sync issues."});
            raise(CheckIssue::Severity::WARN);
        }
    }
    return worst;
}

std::vector<CheckIssue> VerifyOutput(AVFormatContext* write_ctx,
                                     AVFormatContext* read_ctx) {
    std::vector<CheckIssue> issues;
    if (!write_ctx || !read_ctx) return issues;

    if (write_ctx->nb_streams != read_ctx->nb_streams) {
        issues.push_back({CheckIssue::Severity::FAIL, "container",
            "Stream count differs: wrote " + std::to_string(write_ctx->nb_streams) +
            ", read back " + std::to_string(read_ctx->nb_streams),
            "Output is structurally invalid."});
        return issues;
    }

    for (unsigned i = 0; i < write_ctx->nb_streams; ++i) {
        AVStream* w = write_ctx->streams[i];
        AVStream* r = read_ctx->streams[i];
        std::string desc = "output stream#" + std::to_string(i);

        if (w->codecpar->codec_id != r->codecpar->codec_id) {
            issues.push_back({CheckIssue::Severity::FAIL, desc, "Codec changed on read-back.", "Report as a bug."});
            continue;
        }

        int64_t w_dur_us = (w->duration > 0) ? av_rescale_q(w->duration, w->time_base, {1, 1000000}) : 0;
        int64_t r_dur_us = (r->duration > 0) ? av_rescale_q(r->duration, r->time_base, {1, 1000000}) : 0;

        if (w_dur_us > 0 && r_dur_us > 0) {
            int64_t diff = std::abs(w_dur_us - r_dur_us);
            int64_t tol = std::max<int64_t>(w_dur_us / 20, 200000);
            if (diff > tol) {
                issues.push_back({CheckIssue::Severity::WARN, desc,
                    "Duration mismatch: wrote " + std::to_string(w_dur_us / 1000) +
                    " ms, read back " + std::to_string(r_dur_us / 1000) + " ms",
                    "Player may show a shortened or extended timeline."});
            }
        }
    }
    return issues;
}

uint64_t Fnv1aFdHeadTail(int fd, int64_t size) {
    uint8_t buf[4096];
    uint64_t h = FNV_OFFSET;
    ssize_t n;
    do { n = pread(fd, buf, sizeof(buf), 0); } while (n < 0 && errno == EINTR);
    if (n > 0) h = fnv1a(h, buf, (size_t)n);
    if (size > (int64_t)sizeof(buf)) {
        int64_t tail = size - (int64_t)sizeof(buf);
        do { n = pread(fd, buf, sizeof(buf), tail); } while (n < 0 && errno == EINTR);
        if (n > 0) h = fnv1a(h, buf, (size_t)n);
    }
    h ^= (uint64_t)size;
    h *= FNV_PRIME;
    return h;
}

} // namespace muxnexus