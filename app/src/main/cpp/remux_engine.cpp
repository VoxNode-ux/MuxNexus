#include "remux_engine.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/version.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <android/log.h>
#define TAG "remux_engine"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// FFmpeg 7.0 made the AVIO write buffer const; 6.1 (what build_ffmpeg.sh builds) is not.
#if LIBAVFORMAT_VERSION_MAJOR >= 61
using WriteBuf = const uint8_t;
#else
using WriteBuf = uint8_t;
#endif

namespace {

// Must match RemuxState in TelemetryModel.kt (ordinal order).
constexpr int STATE_READING_HEADERS = 1;
constexpr int STATE_REMUXING = 2;
constexpr int STATE_FINALIZING = 3;
constexpr int STATE_DONE = 4;
constexpr int STATE_ERROR = 5;

struct RemuxJob {
    int input_fd = -1;
    int output_fd = -1;
    char output_format[16] = "mp4";
    std::atomic<bool> cancel_requested{false};

    JavaVM *jvm = nullptr;
    jobject callback_global_ref = nullptr;  // Kotlin (Int, Int, String) -> Unit lambda
    jobject bridge_global_ref = nullptr;    // NativeBridge object (for onTelemetry)
    jclass integer_cls = nullptr;           // global ref
    jmethodID invoke_mid = nullptr;
    jmethodID int_ctor_mid = nullptr;
    jmethodID telemetry_mid = nullptr;
    std::thread worker;
};

struct Outcome {
    int state;
    std::string message;
};

// ---- Custom AVIO straight over the fd handed over by Kotlin (SAF) ----------
// Reads/writes go through pread/pwrite on the raw file descriptor, so no
// bytes ever pass through Java/Kotlin buffers (no GC pressure in the hot path).

struct FdIoState {
    int fd = -1;
    int64_t pos = 0;
};

int fd_read_packet(void *opaque, uint8_t *buf, int buf_size) {
    auto *st = static_cast<FdIoState *>(opaque);
    ssize_t n;
    do { n = pread(st->fd, buf, buf_size, st->pos); } while (n < 0 && errno == EINTR);
    if (n == 0) return AVERROR_EOF;
    if (n < 0) return AVERROR(errno);
    st->pos += n;
    return static_cast<int>(n);
}

int fd_write_packet(void *opaque, WriteBuf *buf, int buf_size) {
    auto *st = static_cast<FdIoState *>(opaque);
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

int64_t fd_seek(void *opaque, int64_t offset, int whence) {
    auto *st = static_cast<FdIoState *>(opaque);
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

// ---- JNI upcalls (thread is attached once in run_remux) ---------------------

void pin_to_performance_core() {
    // Heuristic: big.LITTLE parts usually put the performance cores at the top
    // CPU indexes. A production build should read cpuinfo_max_freq per core.
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned i = n / 2; i < n; ++i) CPU_SET(i, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

JNIEnv *current_env(RemuxJob *job) {
    JNIEnv *env = nullptr;
    job->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    return env;
}

void report_progress(RemuxJob *job, int state, int percent, const char *message) {
    JNIEnv *env = current_env(job);
    if (!env) return;
    jobject state_obj = env->NewObject(job->integer_cls, job->int_ctor_mid, state);
    jobject percent_obj = env->NewObject(job->integer_cls, job->int_ctor_mid, percent);
    jstring message_obj = env->NewStringUTF(message);
    env->CallObjectMethod(job->callback_global_ref, job->invoke_mid, state_obj, percent_obj, message_obj);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(state_obj);
    env->DeleteLocalRef(percent_obj);
    env->DeleteLocalRef(message_obj);
}

void report_telemetry(RemuxJob *job, int64_t pts, int64_t dts, int stream_index,
                      int64_t byte_offset, int frame_size, bool keyframe) {
    JNIEnv *env = current_env(job);
    if (!env) return;
    env->CallVoidMethod(job->bridge_global_ref, job->telemetry_mid,
                        (jlong) pts, (jlong) dts, (jint) stream_index,
                        (jlong) byte_offset, (jint) frame_size, (jboolean) keyframe);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

std::string av_err(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// Frees every FFmpeg object on any exit path from run_remux_impl.
struct Ctx {
    AVFormatContext *in = nullptr;
    AVFormatContext *out = nullptr;
    AVIOContext *in_avio = nullptr;
    AVIOContext *out_avio = nullptr;
    AVPacket *pkt = nullptr;
    std::vector<AVBSFContext *> bsfs;

    ~Ctx() {
        av_packet_free(&pkt);
        for (auto *b : bsfs) if (b) av_bsf_free(&b);
        if (in) avformat_close_input(&in);           // custom IO: pb is left for us to free
        if (in_avio) { av_freep(&in_avio->buffer); avio_context_free(&in_avio); }
        if (out) avformat_free_context(out);         // custom IO: pb is left for us to free
        if (out_avio) { av_freep(&out_avio->buffer); avio_context_free(&out_avio); }
    }
};

// ---- The single-pass remux: read packet -> (optional bsf) -> write packet ---

Outcome run_remux_impl(RemuxJob *job) {
    pin_to_performance_core();

    Ctx c;
    FdIoState in_state{job->input_fd, 0};
    FdIoState out_state{job->output_fd, 0};
    constexpr int IO_BUF_SIZE = 1 << 16;

    report_progress(job, STATE_READING_HEADERS, 0, "Reading input file");

    // ---- input ----
    auto *in_buf = static_cast<uint8_t *>(av_malloc(IO_BUF_SIZE));
    c.in_avio = avio_alloc_context(in_buf, IO_BUF_SIZE, 0, &in_state, fd_read_packet, nullptr, fd_seek);
    if (!c.in_avio) { av_free(in_buf); return {STATE_ERROR, "Out of memory"}; }
    c.in = avformat_alloc_context();
    if (!c.in) return {STATE_ERROR, "Out of memory"};
    c.in->pb = c.in_avio;
    c.in->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&c.in, nullptr, nullptr, nullptr);  // frees c.in and nulls it on failure
    if (ret < 0) return {STATE_ERROR, "Unsupported or damaged file: " + av_err(ret)};
    ret = avformat_find_stream_info(c.in, nullptr);
    if (ret < 0) return {STATE_ERROR, "Can't read stream info: " + av_err(ret)};

    // ---- output ----
    const char *fmt = job->output_format[0] ? job->output_format : "mp4";
    ret = avformat_alloc_output_context2(&c.out, nullptr, fmt, nullptr);
    if (ret < 0 || !c.out) return {STATE_ERROR, std::string("Output format not available: ") + fmt};

    (void) ftruncate(job->output_fd, 0);  // best effort: start from an empty file
    auto *out_buf = static_cast<uint8_t *>(av_malloc(IO_BUF_SIZE));
    c.out_avio = avio_alloc_context(out_buf, IO_BUF_SIZE, 1, &out_state, nullptr, fd_write_packet, fd_seek);
    if (!c.out_avio) { av_free(out_buf); return {STATE_ERROR, "Out of memory"}; }
    c.out->pb = c.out_avio;
    c.out->flags |= AVFMT_FLAG_CUSTOM_IO;
    av_dict_copy(&c.out->metadata, c.in->metadata, 0);

    const bool to_mkv = strcmp(fmt, "matroska") == 0;
    std::vector<int> stream_map(c.in->nb_streams, -1);
    c.bsfs.assign(c.in->nb_streams, nullptr);
    int out_index = 0;

    for (unsigned i = 0; i < c.in->nb_streams; ++i) {
        AVStream *is = c.in->streams[i];
        AVCodecParameters *par = is->codecpar;
        const bool cover_art = (is->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        const bool keep =
            (par->codec_type == AVMEDIA_TYPE_VIDEO && !cover_art) ||
            par->codec_type == AVMEDIA_TYPE_AUDIO ||
            (par->codec_type == AVMEDIA_TYPE_SUBTITLE && to_mkv);
        if (!keep) continue;  // skip data/attachment/cover-art streams the target may reject

        AVStream *os = avformat_new_stream(c.out, nullptr);
        if (!os) return {STATE_ERROR, "Out of memory"};
        avcodec_parameters_copy(os->codecpar, par);
        os->codecpar->codec_tag = 0;  // let the new container pick its own tag
        os->time_base = is->time_base;
        av_dict_copy(&os->metadata, is->metadata, 0);  // keeps language tags
        stream_map[i] = out_index++;

        // Raw ADTS AAC (.aac) must be converted to the "AudioSpecificConfig"
        // form that MP4/MOV/MKV store. This is the one bitstream filter needed.
        if (par->codec_id == AV_CODEC_ID_AAC && strcmp(c.in->iformat->name, "aac") == 0) {
            const AVBitStreamFilter *f = av_bsf_get_by_name("aac_adtstoasc");
            if (f) {
                AVBSFContext *b = nullptr;
                if (av_bsf_alloc(f, &b) < 0) return {STATE_ERROR, "Out of memory"};
                avcodec_parameters_copy(b->par_in, par);
                b->time_base_in = is->time_base;
                ret = av_bsf_init(b);
                if (ret < 0) { av_bsf_free(&b); return {STATE_ERROR, "AAC filter failed: " + av_err(ret)}; }
                avcodec_parameters_copy(os->codecpar, b->par_out);
                os->codecpar->codec_tag = 0;
                c.bsfs[i] = b;
            }
        }
    }
    if (out_index == 0) return {STATE_ERROR, "No audio or video streams found in this file"};

    ret = avformat_write_header(c.out, nullptr);
    if (ret < 0) return {STATE_ERROR, "This codec can't go into " + std::string(fmt) + ": " + av_err(ret)};

    report_progress(job, STATE_REMUXING, 0, "Remuxing");

    const int64_t total_size = avio_size(c.in->pb);
    c.pkt = av_packet_alloc();
    if (!c.pkt) return {STATE_ERROR, "Out of memory"};

    int packet_count = 0;
    int write_errors = 0;
    std::string first_write_error;

    auto emit = [&](AVPacket *p, int in_idx) {
        AVStream *is = c.in->streams[in_idx];
        AVStream *os = c.out->streams[stream_map[in_idx]];
        p->stream_index = stream_map[in_idx];
        av_packet_rescale_ts(p, is->time_base, os->time_base);
        p->pos = -1;

        if ((packet_count & 0x0F) == 0) {  // throttle: every 16th packet is plenty for a UI
            report_telemetry(job, p->pts, p->dts, in_idx, in_state.pos, p->size,
                             (p->flags & AV_PKT_FLAG_KEY) != 0);
        }
        int wr = av_interleaved_write_frame(c.out, p);
        if (wr < 0) {
            if (write_errors++ == 0) first_write_error = av_err(wr);
            LOGE("write_frame failed: %s", av_err(wr).c_str());
        }
        av_packet_unref(p);
        ++packet_count;

        if (total_size > 0 && (packet_count & 0x7F) == 0) {
            int percent = static_cast<int>((in_state.pos * 100) / total_size);
            report_progress(job, STATE_REMUXING, percent > 99 ? 99 : percent, "");
        }
    };

    while (!job->cancel_requested.load()) {
        if (av_read_frame(c.in, c.pkt) < 0) break;  // end of file (or unreadable tail)
        const int idx = c.pkt->stream_index;
        if (idx >= static_cast<int>(stream_map.size()) || stream_map[idx] < 0) {
            av_packet_unref(c.pkt);
            continue;
        }
        if (AVBSFContext *b = c.bsfs[idx]) {
            if (av_bsf_send_packet(b, c.pkt) < 0) { av_packet_unref(c.pkt); continue; }
            while (av_bsf_receive_packet(b, c.pkt) == 0) emit(c.pkt, idx);
        } else {
            emit(c.pkt, idx);
        }
    }

    if (job->cancel_requested.load()) return {STATE_ERROR, "Cancelled"};

    report_progress(job, STATE_FINALIZING, 99, "Finishing file");
    ret = av_write_trailer(c.out);
    avio_flush(c.out->pb);
    if (ret < 0) return {STATE_ERROR, "Couldn't finish the file: " + av_err(ret)};
    if (write_errors > 0) return {STATE_ERROR, "Write failed: " + first_write_error};
    return {STATE_DONE, "Complete"};
}

// Owns the thread's JNI lifetime and both fds handed over from Kotlin.
void run_remux(RemuxJob *job) {
    JNIEnv *env = nullptr;
    if (job->jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        close(job->input_fd);
        close(job->output_fd);
        return;
    }

    Outcome outcome = run_remux_impl(job);  // all FFmpeg objects are freed when this returns

    close(job->input_fd);
    close(job->output_fd);
    job->input_fd = job->output_fd = -1;

    report_progress(job, outcome.state, outcome.state == STATE_DONE ? 100 : 0, outcome.message.c_str());

    env->DeleteGlobalRef(job->callback_global_ref);
    env->DeleteGlobalRef(job->bridge_global_ref);
    env->DeleteGlobalRef(job->integer_cls);
    job->callback_global_ref = job->bridge_global_ref = nullptr;
    job->integer_cls = nullptr;

    job->jvm->DetachCurrentThread();
}

} // namespace

JNIEXPORT jlong JNICALL
Java_com_muxnexus_app_engine_NativeBridge_startRemux(
    JNIEnv *env, jobject thiz,
    jint input_fd, jint output_fd, jstring output_format, jobject on_progress_callback) {

    auto *job = new RemuxJob();
    job->input_fd = input_fd;
    job->output_fd = output_fd;

    if (output_format != nullptr) {
        const char *fmt = env->GetStringUTFChars(output_format, nullptr);
        strncpy(job->output_format, fmt, sizeof(job->output_format) - 1);
        env->ReleaseStringUTFChars(output_format, fmt);
    }

    env->GetJavaVM(&job->jvm);
    job->callback_global_ref = env->NewGlobalRef(on_progress_callback);
    job->bridge_global_ref = env->NewGlobalRef(thiz);

    // Look these up once here (safe: app class loader), reuse on every callback.
    jclass integer_local = env->FindClass("java/lang/Integer");
    job->integer_cls = static_cast<jclass>(env->NewGlobalRef(integer_local));
    env->DeleteLocalRef(integer_local);
    job->int_ctor_mid = env->GetMethodID(job->integer_cls, "<init>", "(I)V");

    jclass lambda_cls = env->GetObjectClass(on_progress_callback);
    job->invoke_mid = env->GetMethodID(
        lambda_cls, "invoke", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(lambda_cls);

    jclass bridge_cls = env->GetObjectClass(thiz);
    job->telemetry_mid = env->GetMethodID(bridge_cls, "onTelemetry", "(JJIJIZ)V");
    env->DeleteLocalRef(bridge_cls);

    job->worker = std::thread(run_remux, job);
    job->worker.detach();
    return reinterpret_cast<jlong>(job);
}

JNIEXPORT void JNICALL
Java_com_muxnexus_app_engine_NativeBridge_cancel(
    JNIEnv *env, jobject thiz, jlong job_handle) {
    if (job_handle == 0 || job_handle == -1) return;  // no job running
    auto *job = reinterpret_cast<RemuxJob *>(job_handle);
    job->cancel_requested.store(true);
}
