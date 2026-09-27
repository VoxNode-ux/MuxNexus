#pragma once
#include <jni.h>
#include <cstdint>

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_offlineremux_app_engine_NativeBridge_startRemux(
    JNIEnv *env, jobject thiz,
    jint input_fd, jint output_fd, jobject on_progress_callback);

JNIEXPORT void JNICALL
Java_com_offlineremux_app_engine_NativeBridge_cancel(
    JNIEnv *env, jobject thiz, jlong job_handle);

}
