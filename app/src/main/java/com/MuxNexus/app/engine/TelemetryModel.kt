package com.muxnexus.app.engine

/**
 * One packet-level telemetry sample surfaced across the JNI bridge.
 * Populated natively in remux_engine.cpp on every packet copied during the
 * single-pass interleave loop; never blocks the native thread (see
 * NativeBridge.onTelemetry — it just enqueues onto a channel).
 */
data class TelemetrySample(
    val ptsUs: Long,
    val dtsUs: Long,
    val streamIndex: Int,
    val byteOffset: Long,
    val frameSize: Int,
    val isKeyframe: Boolean
)

enum class RemuxState {
    IDLE, READING_HEADERS, REMUXING, FINALIZING, DONE, ERROR
}

data class RemuxProgress(
    val state: RemuxState,
    val percent: Int = 0,
    val message: String = "",
    val lastSample: TelemetrySample? = null
)

/** Output container the user picks when queuing a job. */
enum class OutputFormat(
    val label: String,
    val shortLabel: String,
    val ffmpegMuxerName: String,
    val fileExtension: String,
    val mimeType: String
) {
    MP4("MP4 (.mp4)", "MP4", "mp4", "mp4", "video/mp4"),
    MOV("QuickTime (.mov)", "MOV", "mov", "mov", "video/quicktime"),
    MKV("Matroska (.mkv)", "MKV", "matroska", "mkv", "video/x-matroska")
}

enum class JobStatus { PENDING, RUNNING, DONE, ERROR }
 