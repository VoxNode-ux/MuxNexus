#include "../core/remux_core.h"

#include <android/log.h>
#include <cstdarg>
#include <unistd.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>

#define TAG "muxnexus_jni"

namespace muxnexus {
void LogError(const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, tag, fmt, ap);
    va_end(ap);
}
void LogInfo(const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, tag, fmt, ap);
    va_end(ap);
}
} // namespace muxnexus

namespace {

void pin_to_performance_core() {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2) return;
    cpu_set_t set; CPU_ZERO(&set);
    for (unsigned i = n / 2; i < n; ++i) CPU_SET(i, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

struct JniJob {
    JavaVM* jvm = nullptr;
    jobject callback_ref = nullptr;
    jobject bridge_ref = nullptr;
    jmethodID progress_mid = nullptr;   // RemuxProgressCallback.onProgress(IIILjava/lang/String;)V
    jmethodID telemetry_mid = nullptr;  // NativeBridge.onTelemetry(JJIJIZ)V
    std::atomic<bool> cancel{false};
};

class JniCallback : public muxnexus::RemuxCallback {
public:
    explicit JniCallback(JniJob* job) : job_(job) {}
    void on_progress(muxnexus::RemuxState state, int percent, const std::string& msg) override {
        JNIEnv* env = get_env();
        if (!env) return;
        jstring m = env->NewStringUTF(msg.c_str());
        env->CallVoidMethod(job_->callback_ref, job_->progress_mid,
                            (jint)state, (jint)percent, m);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(m);
    }
    void on_telemetry(const muxnexus::TelemetrySample& t) override {
        JNIEnv* env = get_env();
        if (!env) return;
        env->CallVoidMethod(job_->bridge_ref, job_->telemetry_mid,
                            (jlong)t.pts_us, (jlong)t.dts_us, (jint)t.stream_index,
                            (jlong)t.byte_offset, (jint)t.frame_size, (jboolean)t.is_keyframe);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
private:
    JNIEnv* get_env() {
        JNIEnv* env = nullptr;
        job_->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        return env;
    }
    JniJob* job_;
};

std::mutex g_mutex;
std::unordered_map<jlong, std::shared_ptr<JniJob>> g_jobs;
std::atomic<jlong> g_next{1};

void erase_job(jlong id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jobs.erase(id);
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_muxnexus_app_engine_NativeBridge_startRemux(
    JNIEnv* env, jobject thiz, jint input_fd, jint output_fd,
    jstring output_format, jobject on_progress) {
    auto job = std::make_shared<JniJob>();
    env->GetJavaVM(&job->jvm);
    job->callback_ref = env->NewGlobalRef(on_progress);
    job->bridge_ref = env->NewGlobalRef(thiz);

    // Named interface — no Function3, no boxing, R8-safe.
    jclass cb_cls = env->GetObjectClass(on_progress);
    job->progress_mid = env->GetMethodID(cb_cls, "onProgress", "(IILjava/lang/String;)V");
    env->DeleteLocalRef(cb_cls);
    if (!job->progress_mid) {
        env->ExceptionClear();
        env->DeleteGlobalRef(job->callback_ref);
        env->DeleteGlobalRef(job->bridge_ref);
        return -1;  // Kotlin treats -1 as "start failed"
    }

    jclass bridge_cls = env->GetObjectClass(thiz);
    job->telemetry_mid = env->GetMethodID(bridge_cls, "onTelemetry", "(JJIJIZ)V");
    env->DeleteLocalRef(bridge_cls);
    if (!job->telemetry_mid) {
        env->ExceptionClear();
        env->DeleteGlobalRef(job->callback_ref);
        env->DeleteGlobalRef(job->bridge_ref);
        return -1;
    }

    std::string fmt;
    if (output_format) {
        const char* s = env->GetStringUTFChars(output_format, nullptr);
        fmt = s; env->ReleaseStringUTFChars(output_format, s);
    }

    jlong id = g_next.fetch_add(1);
    { std::lock_guard<std::mutex> lock(g_mutex); g_jobs[id] = job; }

    std::thread([job, id, input_fd, output_fd, fmt]() {
        JNIEnv* e = nullptr;
        if (job->jvm->AttachCurrentThread(&e, nullptr) != JNI_OK) {
            close(input_fd);
            close(output_fd);
            erase_job(id);   // <- fix #2: no leak, stale handle now null-safe
            return;
        }
        pin_to_performance_core();
        JniCallback cb(job.get());
        auto result = muxnexus::RunRemux(input_fd, output_fd, fmt, &cb, &job->cancel);

        close(input_fd);
        close(output_fd);

        cb.on_progress(result.state, result.state == muxnexus::RemuxState::DONE ? 100 : 0, result.message);

        e->DeleteGlobalRef(job->callback_ref);
        e->DeleteGlobalRef(job->bridge_ref);
        job->jvm->DetachCurrentThread();
        erase_job(id);
    }).detach();

    return id;
}

extern "C" JNIEXPORT void JNICALL
Java_com_muxnexus_app_engine_NativeBridge_cancel(JNIEnv*, jobject, jlong handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_jobs.find(handle);
    if (it != g_jobs.end()) it->second->cancel.store(true);
}