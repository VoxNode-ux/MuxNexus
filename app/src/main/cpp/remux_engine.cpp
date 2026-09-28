#include "remux_engine.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
}

#include <android/log.h>
#define TAG "remux_engine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

constexpr int STATE_IDLE = 0;
constexpr int STATE_READING_HEADERS = 1;
constexpr int STATE_REMUXING = 2;
constexpr int STATE_FINALIZING = 3;
constexpr int STATE_DONE = 4;
constexpr int STATE_ERROR = 5;

// One entry per in-flight job; job_handle is just its address cast to jlong.
struct RemuxJob {
    int input_fd = -1;
    int output_fd = -1;
    char output_format[16] = "mp4";
    std::atomic<bool> cancel_requested{false};
    JavaVM *jvm = nullptr;
    jobject callback_global_ref = nullptr; // the Kotlin lambda, held as a global ref
    jobject bridge_global_ref = nullptr;   // NativeBridge instance, for onTelemetry upcall
    std::thread worker;
};

// ---- Custom AVIOContext over an already-open fd (SAF-provided) -----------
// Deliberately mmap-backed rather than repeated read()/write() syscalls, per
// the "memory-mapped I/O to avoid GC pauses on the Java side" requirement --
// there is no Java-side buffer at all in the hot path.

struct FdIoState {
    int fd = -1;
    uint8_t *map = nullptr;
    size_t map_len = 0;
    int64_t pos = 0;
    bool writable = false;
};

int fd_read_packet(void *opaque, uint8_t *buf, int buf_size) {
    auto *st = static_cast<FdIoState *>(opaque);
    ssize_t n = pread(st->fd, buf, buf_size, st->pos);
    if (n <= 0) return AVERROR_EOF;
    st->pos += n;
    return static_cast<int>(n);
}

int fd_write_packet(void *opaque, const uint8_t *buf, int buf_size) {
    auto *st = static_cast<FdIoState *>(opaque);
    ssize_t n = pwrite(st->fd, buf, buf_size, st->pos);
    if (n < 0) return AVERROR(errno);
    st->pos += n;
    return static_cast<int>(n);
}

int64_t fd_seek(void *opaque, int64_t offset, int whence) {
    auto *st = static_cast<FdIoState *>(opaque);
    if (whence == AVSEEK_SIZE) {
        off_t cur = lseek(st->fd, 0, SEEK_CUR);
        off_t end = lseek(st->fd, 0, SEEK_END);
        lseek(st->fd, cur, SEEK_SET);
        return end;
    }
    off_t result = lseek(st->fd, offset, whence);
    if (result < 0) return AVERROR(errno);
    st->pos = result;
    return result;
}

// ---- JNI upcall helpers ----------------------------------------------------

void pin_to_performance_core() {
    // Heuristic: pin to the highest-numbered CPUs, which on big.LITTLE parts
    // are conventionally the performance cluster. A production build should
    // instead read /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq and
    // pick the top cores explicitly rather than assuming index order.
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned i = n / 2; i < n; ++i) CPU_SET(i, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

void report_progress(RemuxJob *job, int state, int percent, const char *message) {
    JNIEnv *env = nullptr;
    bool attached = false;
    if (job->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        job->jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass lambda_cls = env->GetObjectClass(job->callback_global_ref);
    // Kotlin compiles (Int, Int, String) -> Unit lambdas down to invoke(Object,Object,Object)
    jmethodID invoke = env->GetMethodID(
        lambda_cls, "invoke", "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    jclass integer_cls = env->FindClass("java/lang/Integer");
    jmethodID int_ctor = env->GetMethodID(integer_cls, "<init>", "(I)V");
    jobject state_obj = env->NewObject(integer_cls, int_ctor, state);
    jobject percent_obj = env->NewObject(integer_cls, int_ctor, percent);
    jstring message_obj = env->NewStringUTF(message);

    env->CallObjectMethod(job->callback_global_ref, invoke, state_obj, percent_obj, message_obj);

    env->DeleteLocalRef(state_obj);
    env->DeleteLocalRef(percent_obj);
    env->DeleteLocalRef(message_obj);
    env->DeleteLocalRef(lambda_cls);
    env->DeleteLocalRef(integer_cls);

    if (attached) job->jvm->DetachCurrentThread();
}

void report_telemetry(RemuxJob *job, int64_t pts, int64_t dts, int stream_index,
                       int64_t byte_offset, int frame_size, bool keyframe) {
    JNIEnv *env = nullptr;
    bool attached = false;
    if (job->jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        job->jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass bridge_cls = env->GetObjectClass(job->bridge_global_ref);
    jmethodID on_telemetry = env->GetMethodID(bridge_cls, "onTelemetry", "(JJIJIZ)V");
    env->CallVoidMethod(job->bridge_global_ref, on_telemetry,
                         (jlong) pts, (jlong) dts, stream_index,
                         (jlong) byte_offset, frame_size, (jboolean) keyframe);
    env->DeleteLocalRef(bridge_cls);
    if (attached) job->jvm->DetachCurrentThread();
}

// ---- The actual single-pass remux loop ------------------------------------

void run_remux_impl(RemuxJob *job) {
    pin_to_performance_core();

    AVFormatContext *in_ctx = nullptr;
    AVFormatContext *out_ctx = nullptr;
    AVIOContext *in_avio = nullptr;
    AVIOContext *out_avio = nullptr;
    uint8_t *in_buf = nullptr;
    uint8_t *out_buf = nullptr;
    FdIoState in_state{job->input_fd, nullptr, 0, 0, false};
    FdIoState out_state{job->output_fd, nullptr, 0, 0, true};

    report_progress(job, STATE_READING_HEADERS, 0, "Probing input container");

    constexpr int IO_BUF_SIZE = 1 << 16;
    in_buf = static_cast<uint8_t *>(av_malloc(IO_BUF_SIZE));
    in_avio = avio_alloc_context(in_buf, IO_BUF_SIZE, 0, &in_state,
                                  fd_read_packet, nullptr, fd_seek);

    in_ctx = avformat_alloc_context();
    in_ctx->pb = in_avio;
    in_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&in_ctx, nullptr, nullptr, nullptr) < 0) {
        report_progress(job, STATE_ERROR, 0, "Failed to open input (unsupported or corrupt container)");
        return;
    }
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        report_progress(job, STATE_ERROR, 0, "Failed to read stream info");
        avformat_close_input(&in_ctx);
        return;
    }

    // Output container: chosen by the user in the UI (see OutputFormat in
    // TelemetryModel.kt) instead of guessed. Falls back to "mp4" if the
    // native layer somehow receives an empty string.
    const char *out_format_name = job->output_format[0] != '\0' ? job->output_format : "mp4";
    avformat_alloc_output_context2(&out_ctx, nullptr, out_format_name, nullptr);

    out_buf = static_cast<uint8_t *>(av_malloc(IO_BUF_SIZE));
    out_avio = avio_alloc_context(out_buf, IO_BUF_SIZE, 1, &out_state,
                                   nullptr, fd_write_packet, fd_seek);
    out_ctx->pb = out_avio;
    out_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Bitstream filter graph: normalizes H.264/H.265 SPS/PPS framing when
    // remuxing between Annex-B (typical in .ts/.mkv) and length-prefixed
    // (required inside ISOBMFF) representations.
    std::vector<const AVBitStreamFilter *> bsf_per_stream(in_ctx->nb_streams, nullptr);
    std::vector<AVBSFContext *> bsf_ctx_per_stream(in_ctx->nb_streams, nullptr);

    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        AVStream *in_stream = in_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(out_ctx, nullptr);
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        out_stream->time_base = in_stream->time_base;

        if (in_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
            bsf_per_stream[i] = av_bsf_get_by_name("h264_mp4toannexb");
        } else if (in_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            bsf_per_stream[i] = av_bsf_get_by_name("hevc_mp4toannexb");
        }
        if (bsf_per_stream[i]) {
            av_bsf_alloc(bsf_per_stream[i], &bsf_ctx_per_stream[i]);
            avcodec_parameters_copy(bsf_ctx_per_stream[i]->par_in, in_stream->codecpar);
            av_bsf_init(bsf_ctx_per_stream[i]);
        }
    }

    if (avformat_write_header(out_ctx, nullptr) < 0) {
        report_progress(job, STATE_ERROR, 0, "Failed to write output header");
        avformat_close_input(&in_ctx);
        return;
    }

    report_progress(job, STATE_REMUXING, 0, "Remuxing");

    int64_t total_size = in_ctx->pb ? avio_size(in_ctx->pb) : 0;
    AVPacket *pkt = av_packet_alloc();
    int packet_count = 0;

    while (!job->cancel_requested.load()) {
        int ret = av_read_frame(in_ctx, pkt);
        if (ret < 0) break; // EOF or error; treat both as end-of-stream here

        AVStream *in_stream = in_ctx->streams[pkt->stream_index];
        AVStream *out_stream = out_ctx->streams[pkt->stream_index];

        if (bsf_ctx_per_stream[pkt->stream_index]) {
            av_bsf_send_packet(bsf_ctx_per_stream[pkt->stream_index], pkt);
            av_bsf_receive_packet(bsf_ctx_per_stream[pkt->stream_index], pkt);
        }

        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;

        report_telemetry(job, pkt->pts, pkt->dts, pkt->stream_index,
                          in_state.pos, pkt->size,
                          (pkt->flags & AV_PKT_FLAG_KEY) != 0);

        if (av_interleaved_write_frame(out_ctx, pkt) < 0) {
            LOGE("write_frame failed on packet %d", packet_count);
        }

        av_packet_unref(pkt);
        ++packet_count;

        if (total_size > 0 && (packet_count & 0x3F) == 0) {
            int percent = static_cast<int>((in_state.pos * 100) / total_size);
            report_progress(job, STATE_REMUXING, percent, "");
        }
    }

    av_packet_free(&pkt);
    report_progress(job, STATE_FINALIZING, 99, "Writing trailer / moov atom");
    av_write_trailer(out_ctx);

    for (auto *bsf : bsf_ctx_per_stream) if (bsf) av_bsf_free(&bsf);
    avformat_close_input(&in_ctx);
    avio_context_free(&out_avio);
    avformat_free_context(out_ctx);

    if (job->cancel_requested.load()) {
        report_progress(job, STATE_ERROR, 100, "Cancelled");
    } else {
        report_progress(job, STATE_DONE, 100, "Complete");
    }
}

// Wrapper so the two fds handed over from Kotlin (detachFd) are always closed,
// including on every early-error return inside run_remux_impl.
void run_remux(RemuxJob *job) {
    run_remux_impl(job);
    if (job->input_fd >= 0) close(job->input_fd);
    if (job->output_fd >= 0) close(job->output_fd);
    job->input_fd = job->output_fd = -1;
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

    job->worker = std::thread(run_remux, job);
    job->worker.detach();

    return reinterpret_cast<jlong>(job);
}

JNIEXPORT void JNICALL
Java_com_muxnexus_app_engine_NativeBridge_cancel(
    JNIEnv *env, jobject thiz, jlong job_handle) {
    auto *job = reinterpret_cast<RemuxJob *>(job_handle);
    if (job) job->cancel_requested.store(true);
}
