#include "remux_core.h"
#include "remux_checks.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <unistd.h>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
}

namespace muxnexus {

#if LIBAVFORMAT_VERSION_MAJOR >= 61
using WriteBuf = const uint8_t;
#else
using WriteBuf = uint8_t;
#endif

namespace {

struct FdIoState {
    int fd = -1;
    int64_t pos = 0;
};

int fd_read_packet(void* opaque, uint8_t* buf, int buf_size) {
    auto* st = static_cast<FdIoState*>(opaque);
    ssize_t n;
    do { n = pread(st->fd, buf, buf_size, st->pos); } while (n < 0 && errno == EINTR);
    if (n == 0) return AVERROR_EOF;
    if (n < 0) return AVERROR(errno);
    st->pos += n;
    return static_cast<int>(n);
}

int fd_write_packet(void* opaque, WriteBuf* buf, int buf_size) {
    auto* st = static_cast<FdIoState*>(opaque);
    int total = 0;
    while (total < buf_size) {
        ssize_t n = pwrite(st->fd, buf + total, buf_size - total, st->pos);
        if (n < 0) {
            if (errno == EINTR) continue;
            return AVERROR(errno);
        }
        st->pos += n;
        total += static_cast<int>(n);
    }
    return total;
}

int64_t fd_seek(void* opaque, int64_t offset, int whence) {
    auto* st = static_cast<FdIoState*>(opaque);
    whence &= ~AVSEEK_FORCE;
    struct stat sb;
    if (whence == AVSEEK_SIZE) {
        if (fstat(st->fd, &sb) < 0) return AVERROR(errno);
        return sb.st_size;
    }
    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = st->pos; break;
        case SEEK_END:
            if (fstat(st->fd, &sb) < 0) return AVERROR(errno);
            base = sb.st_size;
            break;
        default: return AVERROR(EINVAL);
    }
    int64_t target = base + offset;
    if (target < 0) return AVERROR(EINVAL);
    st->pos = target;
    return target;
}

std::string av_err(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

int64_t to_us(int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE) return INT64_MIN;
    int64_t out = av_rescale_q(ts, tb, AVRational{1, 1000000});
    return out == AVERROR(ERANGE) ? INT64_MIN : out;
}

struct Ctx {
    AVFormatContext* in = nullptr;
    AVFormatContext* out = nullptr;
    AVIOContext* in_avio = nullptr;
    AVIOContext* out_avio = nullptr;
    AVPacket* pkt = nullptr;
    std::vector<AVBSFContext*> bsfs;

    ~Ctx() {
        av_packet_free(&pkt);
        for (auto* b : bsfs) if (b) av_bsf_free(&b);
        if (in) avformat_close_input(&in);
        if (in_avio) { av_freep(&in_avio->buffer); avio_context_free(&in_avio); }
        if (out) avformat_free_context(out);
        if (out_avio) { av_freep(&out_avio->buffer); avio_context_free(&out_avio); }
    }
};

} // namespace

RemuxResult RunRemux(int input_fd, int output_fd,
                     const std::string& output_format,
                     RemuxCallback* cb, std::atomic<bool>* cancel,
                     const RemuxOptions& options) {
    RemuxResult result;
    Ctx c;
    FdIoState in_state{input_fd, 0};
    FdIoState out_state{output_fd, 0};

    const int buf_size = std::max(4096, options.io_buffer_size);
    const auto job_start = std::chrono::steady_clock::now();
    const int64_t max_consecutive_errors =
        std::max<int64_t>(1, options.max_consecutive_read_errors);

    if (options.fingerprint_input) {
        Fingerprint fp = FingerprintFd(input_fd);
        if (fp.kind == ContainerKind::UNKNOWN) {
            LogInfo("remux", "Input does not match a known container; letting FFmpeg try.");
        } else {
            LogInfo("remux", "Detected container: %s (%d%%)", fp.label.c_str(), fp.confidence);
        }
    }

    if (cb) cb->on_progress(RemuxState::READING_HEADERS, 0, "Reading input file");

    auto* in_buf = static_cast<uint8_t*>(av_malloc(buf_size));
    c.in_avio = avio_alloc_context(in_buf, buf_size, 0, &in_state,
                                   fd_read_packet, nullptr, fd_seek);
    if (!c.in_avio) { av_free(in_buf); result.state = RemuxState::ERROR; result.message = "Out of memory"; return result; }
    c.in = avformat_alloc_context();
    if (!c.in) { result.state = RemuxState::ERROR; result.message = "Out of memory"; return result; }
    c.in->pb = c.in_avio;
    c.in->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&c.in, nullptr, nullptr, nullptr);
    if (ret < 0) { result.state = RemuxState::ERROR; result.message = "Unsupported or damaged file: " + av_err(ret); return result; }
    ret = avformat_find_stream_info(c.in, nullptr);
    if (ret < 0) { result.state = RemuxState::ERROR; result.message = "Can't read stream info: " + av_err(ret); return result; }

    const char* fmt = output_format.empty() ? "mp4" : output_format.c_str();
    ret = avformat_alloc_output_context2(&c.out, nullptr, fmt, nullptr);
    if (ret < 0 || !c.out) {
        result.state = RemuxState::ERROR;
        result.message = std::string("Output format not available: ") + fmt;
        return result;
    }

    (void)ftruncate(output_fd, 0);
    auto* out_buf = static_cast<uint8_t*>(av_malloc(buf_size));
    c.out_avio = avio_alloc_context(out_buf, buf_size, 1, &out_state,
                                    nullptr, fd_write_packet, fd_seek);
    if (!c.out_avio) { av_free(out_buf); result.state = RemuxState::ERROR; result.message = "Out of memory"; return result; }
    c.out->pb = c.out_avio;
    c.out->flags |= AVFMT_FLAG_CUSTOM_IO;
    av_dict_copy(&c.out->metadata, c.in->metadata, 0);

    const bool to_mp4_or_mov = strcmp(fmt, "mp4") == 0 || strcmp(fmt, "mov") == 0;
    const bool to_mkv = strcmp(fmt, "matroska") == 0;

    std::vector<int> stream_map(c.in->nb_streams, -1);
    std::vector<bool> has_bsf(c.in->nb_streams, false);
    c.bsfs.assign(c.in->nb_streams, nullptr);
    int out_index = 0;
    bool any_fail = false;

    for (unsigned i = 0; i < c.in->nb_streams; ++i) {
        AVStream* is = c.in->streams[i];
        AVCodecParameters* par = is->codecpar;
        const bool cover_art = (is->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        const bool keep =
            (par->codec_type == AVMEDIA_TYPE_VIDEO && !cover_art) ||
            par->codec_type == AVMEDIA_TYPE_AUDIO ||
            (par->codec_type == AVMEDIA_TYPE_SUBTITLE && to_mkv);
        if (!keep) continue;

        if (options.check_codec_matrix) {
            CompatResult cr = CheckCodecContainer(par->codec_id, fmt);
            if (cr.rule == CompatRule::FAIL) {
                result.issues.push_back({2, "stream#" + std::to_string(i), cr.note, cr.suggestion});
                any_fail = true;
                continue;
            }
            if (cr.rule == CompatRule::WARN || cr.rule == CompatRule::ALLOW_WITH_NOTE) {
                result.issues.push_back({1, "stream#" + std::to_string(i), cr.note, cr.suggestion});
            }
        }

        AVStream* os = avformat_new_stream(c.out, nullptr);
        if (!os) { result.state = RemuxState::ERROR; result.message = "Out of memory"; return result; }
        avcodec_parameters_copy(os->codecpar, par);
        os->codecpar->codec_tag = 0;
        os->time_base = is->time_base;
        av_dict_copy(&os->metadata, is->metadata, 0);
        stream_map[i] = out_index++;

        if (to_mp4_or_mov && par->codec_id == AV_CODEC_ID_HEVC) {
            os->codecpar->codec_tag = MKBETAG('h', 'v', 'c', '1');
            const AVBitStreamFilter* f = av_bsf_get_by_name("hevc_mp4toannexb");
            if (f) {
                AVBSFContext* b = nullptr;
                if (av_bsf_alloc(f, &b) < 0) {
                    result.state = RemuxState::ERROR; result.message = "Out of memory"; return result;
                }
                avcodec_parameters_copy(b->par_in, par);
                b->time_base_in = is->time_base;
                ret = av_bsf_init(b);
                if (ret < 0) {
                    av_bsf_free(&b);
                    LogError("remux", "HEVC filter init failed (%s); proceeding without filter",
                             av_err(ret).c_str());
                } else {
                    avcodec_parameters_copy(os->codecpar, b->par_out);
                    os->codecpar->codec_tag = MKBETAG('h', 'v', 'c', '1');
                    c.bsfs[i] = b;
                    has_bsf[i] = true;
                }
            }
        }

        if (par->codec_id == AV_CODEC_ID_AAC && strcmp(c.in->iformat->name, "aac") == 0) {
            const AVBitStreamFilter* f = av_bsf_get_by_name("aac_adtstoasc");
            if (f) {
                AVBSFContext* b = nullptr;
                if (av_bsf_alloc(f, &b) < 0) { result.state = RemuxState::ERROR; result.message = "Out of memory"; return result; }
                avcodec_parameters_copy(b->par_in, par);
                b->time_base_in = is->time_base;
                ret = av_bsf_init(b);
                if (ret < 0) { av_bsf_free(&b); result.state = RemuxState::ERROR; result.message = "AAC filter failed: " + av_err(ret); return result; }
                avcodec_parameters_copy(os->codecpar, b->par_out);
                os->codecpar->codec_tag = 0;
                c.bsfs[i] = b;
                has_bsf[i] = true;
            }
        }
    }

    if (out_index == 0) {
        result.state = RemuxState::ERROR;
        result.message = any_fail
            ? "No compatible streams for the selected output format."
            : "No audio or video streams found.";
        return result;
    }

    ret = avformat_write_header(c.out, nullptr);
    if (ret < 0) {
        result.state = RemuxState::ERROR;
        result.message = "This codec can't go into " + std::string(fmt) + ": " + av_err(ret);
        return result;
    }

    RemuxChecks checks;
    checks.attach(c.in, c.out, stream_map, has_bsf, fmt);

    if (cb) cb->on_progress(RemuxState::REMUXING, 0, "Remuxing");

    const int64_t total_size = avio_size(c.in->pb);
    c.pkt = av_packet_alloc();
    if (!c.pkt) { result.state = RemuxState::ERROR; result.message = "Out of memory"; return result; }

    result.checkpoint.stream_last_dts_us.assign(c.in->nb_streams, -1);

    int packet_count = 0;
    int64_t consecutive_read_errors = 0;
    bool input_damaged = false;
    int write_errors = 0;
    std::string first_write_error;

    auto emit = [&](AVPacket* p, int in_idx) {
        AVStream* is = c.in->streams[in_idx];
        AVStream* os = c.out->streams[stream_map[in_idx]];
        p->stream_index = stream_map[in_idx];
        av_packet_rescale_ts(p, is->time_base, os->time_base);
        p->pos = -1;

        int64_t pts_us = to_us(p->pts, os->time_base);
        int64_t dts_us = to_us(p->dts, os->time_base);

        if ((packet_count & 0x0F) == 0 && cb) {
            cb->on_telemetry({pts_us, dts_us, in_idx, in_state.pos, p->size,
                              (p->flags & AV_PKT_FLAG_KEY) != 0});
        }

        int wr = av_interleaved_write_frame(c.out, p);
        if (wr < 0) {
            if (write_errors++ == 0) first_write_error = av_err(wr);
        } else {
            checks.on_packet_out(in_idx, dts_us, pts_us, p->size,
                                 (p->flags & AV_PKT_FLAG_KEY) != 0);
            result.checkpoint.stream_last_dts_us[in_idx] = dts_us;
        }
        av_packet_unref(p);
        ++packet_count;

        if (cb && (packet_count & 0x7F) == 0) {
            int percent = 0;
            if (options.emit_content_progress) {
                percent = checks.content_progress_us();
            } else if (total_size > 0) {
                percent = static_cast<int>((in_state.pos * 100LL) / total_size);
                if (percent > 99) percent = 99;
            }
            cb->on_progress(RemuxState::REMUXING, percent, "");
        }
    };

    while (!(cancel && cancel->load())) {
        // Wall-clock guard: cannot hang forever.
        if ((packet_count & 0xFFF) == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - job_start);
            if (elapsed.count() > options.max_job_seconds) {
                result.state = RemuxState::ERROR;
                result.message = "Job exceeded " + std::to_string(options.max_job_seconds) +
                                 " s wall-clock limit; aborted";
                return result;
            }
        }

        int read_ret = av_read_frame(c.in, c.pkt);
        if (read_ret == AVERROR_EOF) break;
        if (read_ret < 0) {
            LogError("remux", "Unreadable data near byte %lld: %s",
                     (long long)in_state.pos, av_err(read_ret).c_str());
            if (++consecutive_read_errors > max_consecutive_errors) {
                input_damaged = true;
                break;
            }
            continue;
        }
        consecutive_read_errors = 0;

        int idx = c.pkt->stream_index;
        if (idx < 0 || idx >= (int)stream_map.size() || stream_map[idx] < 0) {
            av_packet_unref(c.pkt);
            continue;
        }

        AVStream* is = c.in->streams[idx];
        int64_t pts_us = to_us(c.pkt->pts, is->time_base);
        int64_t dts_us = to_us(c.pkt->dts, is->time_base);
        checks.on_packet_in(idx, dts_us, pts_us, c.pkt->size,
                            (c.pkt->flags & AV_PKT_FLAG_KEY) != 0);

        if (AVBSFContext* b = c.bsfs[idx]) {
            if (av_bsf_send_packet(b, c.pkt) < 0) { av_packet_unref(c.pkt); continue; }
            while (av_bsf_receive_packet(b, c.pkt) == 0) emit(c.pkt, idx);
        } else {
            emit(c.pkt, idx);
        }
    }

    if (cancel && cancel->load()) {
        for (unsigned i = 0; i < c.bsfs.size(); ++i) {
            AVBSFContext* b = c.bsfs[i];
            if (!b) continue;
            if (av_bsf_send_packet(b, nullptr) < 0) continue;
            while (av_bsf_receive_packet(b, c.pkt) == 0) emit(c.pkt, (int)i);
        }
        av_write_trailer(c.out);
        avio_flush(c.out->pb);
        result.state = RemuxState::CANCELLED;
        result.message = "Cancelled";
        if (options.compute_checkpoint) {
            result.checkpoint.valid = true;
            result.checkpoint.input_hash = Fnv1aFdHeadTail(input_fd, total_size);
            result.checkpoint.output_size = avio_tell(c.out->pb);
        }
        return result;
    }

    for (unsigned i = 0; i < c.bsfs.size(); ++i) {
        AVBSFContext* b = c.bsfs[i];
        if (!b) continue;
        if (av_bsf_send_packet(b, nullptr) < 0) continue;
        while (av_bsf_receive_packet(b, c.pkt) == 0) emit(c.pkt, (int)i);
    }

    if (cb) cb->on_progress(RemuxState::FINALIZING, 99, "Finishing file");
    ret = av_write_trailer(c.out);
    avio_flush(c.out->pb);
    if (ret < 0) {
        result.state = RemuxState::ERROR;
        result.message = "Couldn't finish the file: " + av_err(ret);
        return result;
    }

    std::vector<CheckIssue> inv_issues;
    checks.check_invariants(&inv_issues);
    for (const auto& i : inv_issues) {
        result.issues.push_back({(int)i.severity, i.stream_desc, i.message, i.suggestion});
    }

    // Only true after the output was re-opened and checked without failures.
    bool verified = false;
    bool verify_ran = false;
    const int64_t output_size = avio_tell(c.out->pb);
    const bool should_verify = options.verify_output &&
        !(options.skip_verification_for_large_outputs &&
          output_size > options.skip_verification_above_bytes);

    if (should_verify) {
        int verify_fd = dup(output_fd);
        if (verify_fd >= 0) {
            FdIoState v_state{verify_fd, 0};
            AVFormatContext* v_ctx = nullptr;
            AVIOContext* v_avio = nullptr;
            auto* v_buf = static_cast<uint8_t*>(av_malloc(buf_size));
            if (v_buf) {
                v_avio = avio_alloc_context(v_buf, buf_size, 0, &v_state,
                                            fd_read_packet, nullptr, fd_seek);
            }
            if (v_avio) {
                v_ctx = avformat_alloc_context();
                if (v_ctx) {
                    v_ctx->pb = v_avio;
                    v_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
                    if (avformat_open_input(&v_ctx, nullptr, nullptr, nullptr) >= 0) {
                        if (avformat_find_stream_info(v_ctx, nullptr) >= 0) {
                            verify_ran = true;
                            verified = true;
                            auto v_issues = VerifyOutput(c.out, v_ctx);
                            for (const auto& i : v_issues) {
                                result.issues.push_back({(int)i.severity, i.stream_desc, i.message, i.suggestion});
                                if (i.severity == CheckIssue::Severity::FAIL) verified = false;
                            }
                            result.output_verified = verified;
                        }
                        avformat_close_input(&v_ctx);
                    }
                    if (v_ctx) avformat_free_context(v_ctx);
                }
                av_freep(&v_avio->buffer);
                avio_context_free(&v_avio);
            }
            close(verify_fd);
        }
    } else {
        // Verification skipped: still honest about it, don't claim verified.
        result.output_verified = false;
    }

    if (write_errors > 0) {
        result.state = RemuxState::ERROR;
        result.message = "Write failed: " + first_write_error;
    } else if (input_damaged) {
        result.state = RemuxState::ERROR;
        result.message = "Input file is damaged; saved what could be read";
    } else {
        result.state = RemuxState::DONE;
        // Exact strings: the Kotlin side compares against "Complete".
        result.message = verified ? "Complete"
            : verify_ran ? "Complete (verification warnings)"
                         : "Complete (not verified)";
    }
    return result;
}

} // namespace muxnexus