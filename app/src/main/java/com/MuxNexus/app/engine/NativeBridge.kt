package com.offlineremux.app.engine

import android.os.ParcelFileDescriptor
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.receiveAsFlow

/**
 * Thin JNI boundary. Every call here crosses into remux_engine.cpp.
 *
 * File descriptors, not paths: `openFd` is expected to be a dup'd fd pulled
 * from a SAF Uri via ContentResolver.openFileDescriptor(uri, mode), so the
 * native side never needs java.io.File / generic filesystem access — it
 * reads and writes through the fd:// path the OS/IO tier is built around.
 */
object NativeBridge {

    init {
        // Must match add_library() name in CMakeLists.txt
        System.loadLibrary("remux_engine")
    }

    // Backpressure-free channel so the native thread never blocks on a slow
    // Compose recomposition; UI drops frames of telemetry under load, never
    // the engine.
    private val telemetryChannel = Channel<TelemetrySample>(capacity = Channel.CONFLATED)
    val telemetryFlow = telemetryChannel.receiveAsFlow()

    /**
     * Starts a single-pass remux from [inputFd] to [outputFd]. Returns
     * immediately; progress and completion arrive via [onProgress].
     * Runs on a pinned high-performance core via pthread affinity set
     * natively (see remux_engine.cpp: pin_to_performance_core()).
     */
    external fun startRemux(
        inputFd: Int,
        outputFd: Int,
        outputFormat: String,
        onProgress: (state: Int, percent: Int, message: String) -> Unit
    ): Long // returns an opaque native job handle, or -1 on immediate failure

    external fun cancel(jobHandle: Long)

    /** Called from native code via JNI upcall; kept package-visible on purpose. */
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

    fun withDupedFd(pfd: ParcelFileDescriptor): Int = pfd.detachFd()
}
