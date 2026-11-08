package com.muxnexus.app.engine

import android.os.ParcelFileDescriptor
import androidx.annotation.Keep
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.receiveAsFlow

/**
 * Thin JNI boundary. Every call here crosses into native/android/jni_bridge.cpp.
 * File descriptors, not paths: `detachFd` transfers ownership of a dup'd fd
 * pulled from a SAF Uri via ContentResolver.openFileDescriptor(uri, mode).
 */
object NativeBridge {

    init {
        // Must match add_library() name in native/CMakeLists.txt
        System.loadLibrary("remux_engine")
    }

    private val telemetryChannel = Channel<TelemetrySample>(capacity = Channel.CONFLATED)
    val telemetryFlow = telemetryChannel.receiveAsFlow()

    /**
     * Starts a remux job. Returns the native handle, or -1 if the JVM could
     * not set up the callback (extremely rare; treat as "start failed").
     */
    external fun startRemux(
        inputFd: Int,
        outputFd: Int,
        outputFormat: String,
        onProgress: RemuxProgressCallback
    ): Long

    external fun cancel(jobHandle: Long)

    @Keep
    @Suppress("unused")
    private fun onTelemetry(
        ptsUs: Long,
        dtsUs: Long,
        streamIndex: Int,
        byteOffset: Long,
        frameSize: Int,
        isKeyframe: Boolean
    ) {
        telemetryChannel.trySend(
            TelemetrySample(ptsUs, dtsUs, streamIndex, byteOffset, frameSize, isKeyframe)
        )
    }

    /** Transfers ownership of the fd. The PFD must not be closed afterward. */
    fun detachFd(pfd: ParcelFileDescriptor): Int = pfd.detachFd()
}