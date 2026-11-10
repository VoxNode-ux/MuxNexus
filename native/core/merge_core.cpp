#include "merge_core.h"
#include "remux_checks.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
}

namespace muxnexus {

#if LIBAVFORMAT_VERSION_MAJOR >= 61
using WriteBuf = const uint8_t;
#else
using WriteBuf = uint8_t;
#endif

namespace {

// ---- file-descriptor I/O (same behaviour as remux_core.cpp) ----------------

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

// ---- one opened input ------------------------------------------------------

struct Input {
    FdIoState st;                   // must not move: AVIO keeps a pointer to it
    AVIOContext* avio = nullptr;
    AVFormatContext* fmt = nullptr;
    AVPacket* pkt = nullptr;
    std::vector<int> map;           // input stream index -> output index, -1 = dropped
    int mapped = 0;                 // number of streams written to the output
    bool supplies_video = false;
    int64_t shift_us = 0;           // added to every timestamp of this input
    bool eof = false;               // nothing more will be read
    bool have = false;              // `pkt` holds a packet waiting to be written
    int64_t t_us = 0;               // merged-timeline time of the pending packet
    int64_t last_t_us = INT64_MIN;  // merged-timeline time of the last written packet
    int64_t last_end_us = INT64_MIN; // ... plus that packet's duration (0 if unknown)
    int64_t errors = 0;             // consecutive read errors

    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    ~Input() {
        av_packet_free(&pkt);
        if (fmt) avformat_close_input(&fmt);
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    }
};

struct OutCtx {
    AVFormatContext* fmt = nullptr;
    AVIOContext* avio = nullptr;
    ~OutCtx() {
        if (fmt) avformat_free_context(fmt);
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    }
};

bool open_input(Input& in, int fd, int buf_size, std::string* err) {
    in.st = FdIoState{fd, 0};
    auto* buf = static_cast<uint8_t*>(av_malloc(buf_size));
    if (!buf) { *err = "Out of memory"; return false; }
    in.avio = avio_alloc_context(buf, buf_size, 0, &in.st, fd_read_packet, nullptr, fd_seek);
    if (!in.avio) { av_free(buf); *err = "Out of memory"; return false; }
    in.fmt = avformat_alloc_context();
    if (!in.fmt) { *err = "Out of memory"; return false; }
    in.fmt->pb = in.avio;
    in.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    int ret = avformat_open_input(&in.fmt, nullptr, nullptr, nullptr);
    if (ret < 0) { *err = "Unsupported or damaged file: " + av_err(ret); return false; }
    ret = avformat_find_stream_info(in.fmt, nullptr);
    if (ret < 0) { *err = "Can't read stream info: " + av_err(ret); return false; }
    return true;
}

// ---- stream description ----------------------------------------------------

bool is_lossless(const AVCodecParameters* p) {
    const AVCodecDescriptor* d = avcodec_descriptor_get(p->codec_id);
    if (!d) return false;
    const bool lossless = (d->props & AV_CODEC_PROP_LOSSLESS) != 0;
    const bool lossy = (d->props & AV_CODEC_PROP_LOSSY) != 0;
    if (lossless && !lossy) return true;
    if (!lossless) return false;
    // Codecs that can be either (DTS, WavPack, ...). DTS is lossless only as
    // DTS-HD Master Audio (profile 60 = AV_PROFILE_DTS_HD_MA).
    if (p->codec_id == AV_CODEC_ID_DTS) return p->profile == 60;
    return true;
}

StreamInfo make_info(const AVStream* s) {
    StreamInfo si;
    const AVCodecParameters* p = s->codecpar;
    si.index = s->index;
    switch (p->codec_type) {
        case AVMEDIA_TYPE_VIDEO:    si.type = StreamType::VIDEO; break;
        case AVMEDIA_TYPE_AUDIO:    si.type = StreamType::AUDIO; break;
        case AVMEDIA_TYPE_SUBTITLE: si.type = StreamType::SUBTITLE; break;
        default:                    si.type = StreamType::OTHER; break;
    }
    const char* name = avcodec_get_name(p->codec_id);
    si.codec = name ? name : "unknown";
    si.bit_rate = p->bit_rate;
    si.is_default = (s->disposition & AV_DISPOSITION_DEFAULT) != 0;
    si.is_attached_pic = (s->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
    if (const AVDictionaryEntry* e = av_dict_get(s->metadata, "language", nullptr, 0)) si.language = e->value;
    if (const AVDictionaryEntry* e = av_dict_get(s->metadata, "title", nullptr, 0)) si.title = e->value;
    if (s->duration != AV_NOPTS_VALUE) {
        int64_t d = to_us(s->duration, s->time_base);
        si.duration_us = d == INT64_MIN ? -1 : d;
    }
    if (si.type == StreamType::VIDEO) {
        si.width = p->width;
        si.height = p->height;
    } else if (si.type == StreamType::AUDIO) {
        si.channels = p->ch_layout.nb_channels;
        si.sample_rate = p->sample_rate;
        si.bit_depth = p->bits_per_raw_sample > 0 ? p->bits_per_raw_sample
                                                  : p->bits_per_coded_sample;
        si.lossless = is_lossless(p);
        si.quality_score = AudioQualityScore(si);
    }
    return si;
}

std::string stream_label(size_t input_number, int stream_index) {
    return "input" + std::to_string(input_number) + "/stream#" + std::to_string(stream_index);
}

std::string seconds_text(int64_t us) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(us) / 1e6);
    return buf;
}

StreamPick make_pick(StreamPick::Kind kind) {
    StreamPick p;
    p.kind = kind;
    return p;
}

} // namespace

// ---- public: ranking and probing -------------------------------------------

int64_t AudioQualityScore(const StreamInfo& s) {
    if (s.type != StreamType::AUDIO) return 0;
    int64_t score;
    if (s.lossless) {
        const int64_t depth = s.bit_depth > 0 ? s.bit_depth : 16;
        score = 1000000 + depth * static_cast<int64_t>(s.sample_rate) / 1000;
        score += static_cast<int64_t>(s.channels) * 10;
    } else {
        score = std::min<int64_t>(s.bit_rate / 1000, 99999) * 10;
        score += s.channels;
    }
    return score;
}

int PickBestAudio(const std::vector<StreamInfo>& streams) {
    int best = -1;
    int64_t best_score = INT64_MIN;
    for (size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].type != StreamType::AUDIO) continue;
        const int64_t sc = AudioQualityScore(streams[i]);
        if (best < 0 || sc > best_score) {
            best = static_cast<int>(i);
            best_score = sc;
        }
    }
    return best;
}

ProbeResult ProbeFd(int fd, int io_buffer_size) {
    ProbeResult r;
    Input in;
    std::string err;
    if (!open_input(in, fd, std::max(4096, io_buffer_size), &err)) {
        r.error = err;
        return r;
    }
    r.ok = true;
    r.format_name = in.fmt->iformat ? in.fmt->iformat->name : "";
    r.duration_us = in.fmt->duration != AV_NOPTS_VALUE ? in.fmt->duration : -1;
    for (unsigned i = 0; i < in.fmt->nb_streams; ++i) {
        r.streams.push_back(make_info(in.fmt->streams[i]));
    }
    return r;
}

// ---- public: merge ---------------------------------------------------------

RemuxResult RunMerge(const std::vector<MergeInput>& inputs, int output_fd,
                     const std::string& output_format,
                     RemuxCallback* cb, std::atomic<bool>* cancel,
                     const MergeOptions& options) {
    RemuxResult result;
    auto fail = [&](const std::string& msg) {
        result.state = RemuxState::ERROR;
        result.message = msg;
        return result;
    };

    if (inputs.empty()) return fail("No input files");
    if (output_fd < 0) return fail("No output file");

    const int buf_size = std::max(4096, options.io_buffer_size);
    const auto job_start = std::chrono::steady_clock::now();
    const int64_t max_errors = std::max<int64_t>(1, options.max_consecutive_read_errors);
    const char* fmt_name = output_format.empty() ? "matroska" : output_format.c_str();
    const bool to_mp4_or_mov = strcmp(fmt_name, "mp4") == 0 || strcmp(fmt_name, "mov") == 0;
    const bool to_mkv = strcmp(fmt_name, "matroska") == 0;

    if (cb) cb->on_progress(RemuxState::READING_HEADERS, 0, "Reading input files");

    // 1. Open every input.
    std::vector<std::unique_ptr<Input>> ins;
    for (size_t i = 0; i < inputs.size(); ++i) {
        auto in = std::make_unique<Input>();
        std::string err;
        if (!open_input(*in, inputs[i].fd, buf_size, &err)) {
            return fail("Input " + std::to_string(i + 1) + ": " + err);
        }
        in->pkt = av_packet_alloc();
        if (!in->pkt) return fail("Out of memory");
        ins.push_back(std::move(in));
    }

    // 2. Create the output.
    OutCtx oc;
    FdIoState out_state{output_fd, 0};
    int ret = avformat_alloc_output_context2(&oc.fmt, nullptr, fmt_name, nullptr);
    if (ret < 0 || !oc.fmt) return fail(std::string("Output format not available: ") + fmt_name);
    (void)ftruncate(output_fd, 0);
    auto* out_buf = static_cast<uint8_t*>(av_malloc(buf_size));
    if (!out_buf) return fail("Out of memory");
    oc.avio = avio_alloc_context(out_buf, buf_size, 1, &out_state, nullptr, fd_write_packet, fd_seek);
    if (!oc.avio) { av_free(out_buf); return fail("Out of memory"); }
    oc.fmt->pb = oc.avio;
    oc.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    // 3. Decide which streams go where.
    int out_count = 0;
    bool any_rejected = false;
    for (size_t i = 0; i < ins.size(); ++i) {
        Input& in = *ins[i];
        const MergeInput& spec = inputs[i];
        const size_t n = in.fmt->nb_streams;
        in.map.assign(n, -1);

        std::vector<StreamPick> picks = spec.picks;
        if (picks.empty()) {
            picks.push_back(make_pick(StreamPick::Kind::VIDEO));
            picks.push_back(make_pick(StreamPick::Kind::AUDIO));
            if (to_mkv) picks.push_back(make_pick(StreamPick::Kind::SUBTITLE));
        }

        std::set<int> chosen;
        for (const StreamPick& pk : picks) {
            switch (pk.kind) {
                case StreamPick::Kind::VIDEO:
                case StreamPick::Kind::AUDIO:
                case StreamPick::Kind::SUBTITLE: {
                    const AVMediaType want =
                        pk.kind == StreamPick::Kind::VIDEO ? AVMEDIA_TYPE_VIDEO
                        : pk.kind == StreamPick::Kind::AUDIO ? AVMEDIA_TYPE_AUDIO
                                                             : AVMEDIA_TYPE_SUBTITLE;
                    for (size_t k = 0; k < n; ++k) {
                        const AVStream* s = in.fmt->streams[k];
                        if (s->codecpar->codec_type != want) continue;
                        if (want == AVMEDIA_TYPE_VIDEO &&
                            (s->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
                        chosen.insert(static_cast<int>(k));
                    }
                    break;
                }
                case StreamPick::Kind::BEST_AUDIO: {
                    std::vector<StreamInfo> infos;
                    for (size_t k = 0; k < n; ++k) infos.push_back(make_info(in.fmt->streams[k]));
                    const int best = PickBestAudio(infos);
                    if (best < 0) {
                        result.issues.push_back({1, "input" + std::to_string(i + 1),
                                                 "This file has no audio stream to pick.", ""});
                    } else {
                        chosen.insert(best);
                    }
                    break;
                }
                case StreamPick::Kind::INDEX:
                    if (pk.index < 0 || static_cast<size_t>(pk.index) >= n) {
                        return fail("Input " + std::to_string(i + 1) + " has no stream #" +
                                    std::to_string(pk.index));
                    }
                    chosen.insert(pk.index);
                    break;
            }
        }

        for (int idx : chosen) {
            AVStream* is = in.fmt->streams[idx];
            const AVCodecParameters* par = is->codecpar;
            const std::string label = stream_label(i + 1, idx);

            if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO &&
                par->codec_type != AVMEDIA_TYPE_SUBTITLE) {
                result.issues.push_back({1, label, "This stream type can't be copied; skipped.", ""});
                continue;
            }

            CompatResult cr = CheckCodecContainer(par->codec_id, fmt_name);
            if (cr.rule == CompatRule::FAIL) {
                result.issues.push_back({2, label, cr.note, cr.suggestion});
                any_rejected = true;
                continue;
            }
            if (cr.rule == CompatRule::WARN) {
                result.issues.push_back({1, label, cr.note, cr.suggestion});
            } else if (cr.rule == CompatRule::ALLOW_WITH_NOTE) {
                result.issues.push_back({0, label, cr.note, cr.suggestion});
            }

            AVStream* os = avformat_new_stream(oc.fmt, nullptr);
            if (!os) return fail("Out of memory");
            ret = avcodec_parameters_copy(os->codecpar, par);
            if (ret < 0) return fail("Can't copy stream parameters: " + av_err(ret));
            os->codecpar->codec_tag = 0;
            if (to_mp4_or_mov && par->codec_id == AV_CODEC_ID_HEVC) {
                os->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
            }
            os->time_base = is->time_base;
            os->disposition = is->disposition;
            av_dict_copy(&os->metadata, is->metadata, 0);

            in.map[idx] = out_count++;
            in.mapped++;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO) in.supplies_video = true;
        }
    }

    if (out_count == 0) {
        return fail(any_rejected ? "None of the picked streams can be stored in this output format."
                                 : "Nothing to write: no streams were picked.");
    }

    // Exactly one default audio track, whatever the sources said.
    bool first_audio = true;
    for (unsigned k = 0; k < oc.fmt->nb_streams; ++k) {
        AVStream* os = oc.fmt->streams[k];
        if (os->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (first_audio) { os->disposition |= AV_DISPOSITION_DEFAULT; first_audio = false; }
        else os->disposition &= ~AV_DISPOSITION_DEFAULT;
    }

    // Global metadata comes from the first input that supplies video.
    size_t primary = 0;
    bool has_video_input = false, has_donor_input = false;
    int64_t primary_duration_us = 0;
    for (size_t i = 0; i < ins.size(); ++i) {
        if (ins[i]->supplies_video) {
            if (!has_video_input) primary = i;
            has_video_input = true;
            if (ins[i]->fmt->duration != AV_NOPTS_VALUE)
                primary_duration_us = std::max(primary_duration_us, ins[i]->fmt->duration);
        } else if (ins[i]->mapped > 0) {
            has_donor_input = true;
        }
    }
    av_dict_copy(&oc.fmt->metadata, ins[primary]->fmt->metadata, 0);
    const bool trim_donors = options.end_with_video && has_video_input && has_donor_input;

    // Each input starts at time zero on the merged timeline, plus the user's offset.
    for (size_t i = 0; i < ins.size(); ++i) {
        const int64_t start = ins[i]->fmt->start_time != AV_NOPTS_VALUE ? ins[i]->fmt->start_time : 0;
        ins[i]->shift_us = inputs[i].offset_us - start;
        if (ins[i]->mapped == 0) ins[i]->eof = true;   // nothing to read
    }

    ret = avformat_write_header(oc.fmt, nullptr);
    if (ret < 0) {
        return fail(std::string("This combination can't be stored in ") + fmt_name + ": " + av_err(ret));
    }

    // 4. Copy packets, always taking the earliest one across all inputs.
    int64_t packet_count = 0;
    int write_errors = 0;
    std::string first_write_error;
    bool input_damaged = false;

    auto fill = [&](Input& in) {
        while (!in.eof && !in.have) {
            int r = av_read_frame(in.fmt, in.pkt);
            if (r == AVERROR_EOF) { in.eof = true; break; }
            if (r < 0) {
                LogError("merge", "Unreadable data near byte %lld: %s",
                         static_cast<long long>(in.st.pos), av_err(r).c_str());
                if (++in.errors > max_errors) { input_damaged = true; in.eof = true; }
                continue;
            }
            in.errors = 0;
            const int idx = in.pkt->stream_index;
            if (idx < 0 || static_cast<size_t>(idx) >= in.map.size() || in.map[idx] < 0) {
                av_packet_unref(in.pkt);
                continue;
            }
            const AVStream* is = in.fmt->streams[idx];
            const int64_t ts = in.pkt->dts != AV_NOPTS_VALUE ? in.pkt->dts : in.pkt->pts;
            int64_t us = to_us(ts, is->time_base);
            if (us == INT64_MIN) us = in.last_t_us == INT64_MIN ? 0 : in.last_t_us - in.shift_us;
            in.t_us = us + in.shift_us;
            in.have = true;
        }
    };

    auto emit = [&](Input& in) {
        AVPacket* p = in.pkt;
        const int idx = p->stream_index;
        const AVStream* is = in.fmt->streams[idx];
        const int oidx = in.map[idx];
        const AVStream* os = oc.fmt->streams[oidx];
        const int64_t dur_us = p->duration > 0
            ? av_rescale_q(p->duration, is->time_base, AVRational{1, 1000000}) : 0;

        const int64_t shift_tb = av_rescale_q(in.shift_us, AVRational{1, 1000000}, is->time_base);
        if (p->pts != AV_NOPTS_VALUE) p->pts += shift_tb;
        if (p->dts != AV_NOPTS_VALUE) p->dts += shift_tb;
        p->stream_index = oidx;
        av_packet_rescale_ts(p, is->time_base, os->time_base);
        p->pos = -1;

        if (cb && (packet_count & 0xF) == 0) {
            const int64_t pts_us = to_us(p->pts, os->time_base);
            const int64_t dts_us = to_us(p->dts, os->time_base);
            cb->on_telemetry({pts_us, dts_us, idx, in.st.pos, p->size,
                              (p->flags & AV_PKT_FLAG_KEY) != 0});
        }

        const int w = av_interleaved_write_frame(oc.fmt, p);
        if (w < 0) {
            if (write_errors++ == 0) first_write_error = av_err(w);
        }
        av_packet_unref(p);
        in.have = false;
        in.last_t_us = in.t_us;
        in.last_end_us = in.t_us + dur_us;
    };

    if (cb) cb->on_progress(RemuxState::REMUXING, 0, "Merging");

    while (!(cancel && cancel->load())) {
        if ((packet_count & 0xFFF) == 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - job_start).count();
            if (elapsed > options.max_job_seconds) {
                av_write_trailer(oc.fmt);
                return fail("Timed out");
            }
        }
        if (write_errors > 0) break;

        for (auto& up : ins) fill(*up);

        Input* next = nullptr;
        for (auto& up : ins) {
            if (up->have && (!next || up->t_us < next->t_us)) next = up.get();
        }
        if (!next) break;

        if (trim_donors) {
            bool video_done = true;
            for (auto& up : ins) if (up->supplies_video && up->have) video_done = false;
            if (video_done) {
                // The video is over. Keep donor packets that start before the END
                // of the last video frame (not before its start), drop the rest.
                int64_t video_end = INT64_MIN;
                for (auto& up : ins) {
                    if (up->supplies_video) video_end = std::max(video_end, std::max(up->last_t_us, up->last_end_us));
                }
                if (video_end == INT64_MIN || next->t_us >= video_end) break;
            }
        }

        const int64_t t = next->t_us;
        emit(*next);
        ++packet_count;

        if (cb && (packet_count & 0x7F) == 0 && primary_duration_us > 0) {
            const int pct = static_cast<int>(std::clamp<int64_t>(t * 100 / primary_duration_us, 0, 99));
            cb->on_progress(RemuxState::REMUXING, pct, "Merging");
        }
    }

    if (cancel && cancel->load()) {
        av_write_trailer(oc.fmt);
        avio_flush(oc.fmt->pb);
        result.state = RemuxState::CANCELLED;
        result.message = "Cancelled";
        return result;
    }

    // 5. Tell the user what happened to audio that didn't line up with the video.
    if (trim_donors) {
        int64_t video_end = INT64_MIN;
        for (auto& up : ins) {
            if (up->supplies_video) video_end = std::max(video_end, std::max(up->last_t_us, up->last_end_us));
        }
        for (size_t i = 0; i < ins.size(); ++i) {
            const Input& in = *ins[i];
            if (in.supplies_video || in.mapped == 0) continue;
            const std::string who = "input" + std::to_string(i + 1);
            if (in.have) {
                result.issues.push_back({0, who, "Longer than the video; trimmed to the video's length.", ""});
            } else if (in.last_t_us != INT64_MIN && video_end != INT64_MIN &&
                       video_end - in.last_t_us > 1000000) {
                result.issues.push_back({1, who,
                    "Ends " + seconds_text(video_end - in.last_t_us) + " s before the video; the rest will be silent.",
                    "Use a longer audio source, or set a delay."});
            }
        }
    }

    if (cb) cb->on_progress(RemuxState::FINALIZING, 99, "Finishing file");
    ret = av_write_trailer(oc.fmt);
    avio_flush(oc.fmt->pb);
    if (ret < 0) return fail("Couldn't finish the file: " + av_err(ret));

    // 6. Verify by re-opening the output (needs a readable output fd).
    bool verified = false;
    bool verify_ran = false;
    const int64_t output_size = avio_tell(oc.fmt->pb);
    const bool should_verify = options.verify_output &&
                               output_size <= options.skip_verification_above_bytes;
    if (should_verify) {
        const int verify_fd = dup(output_fd);
        if (verify_fd >= 0) {
            Input vin;
            std::string err;
            if (open_input(vin, verify_fd, buf_size, &err)) {
                verify_ran = true;
                verified = true;
                for (const auto& v : VerifyOutput(oc.fmt, vin.fmt)) {
                    result.issues.push_back({static_cast<int>(v.severity), v.stream_desc, v.message, v.suggestion});
                    if (v.severity == CheckIssue::Severity::FAIL) verified = false;
                }
                result.output_verified = verified;
            }
            close(verify_fd);
        }
    }

    if (write_errors > 0) {
        result.state = RemuxState::ERROR;
        result.message = "Write failed: " + first_write_error;
    } else if (input_damaged) {
        result.state = RemuxState::ERROR;
        result.message = "An input file is damaged; saved what could be read";
    } else {
        result.state = RemuxState::DONE;
        // Same strings as RunRemux; the Kotlin side compares against "Complete".
        result.message = verified ? "Complete"
            : verify_ran ? "Complete (verification warnings)"
                         : "Complete (not verified)";
    }
    return result;
}

} // namespace muxnexus