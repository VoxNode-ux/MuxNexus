package com.muxnexus.app.engine

data class TelemetrySample(
    val ptsUs: Long,
    val dtsUs: Long,
    val streamIndex: Int,
    val byteOffset: Long,
    val frameSize: Int,
    val isKeyframe: Boolean
)

/**
 * Ordinal order MUST match the C++ RemuxState enum in native/core/remux_core.h.
 * The JNI bridge passes the raw int; a mismatch here silently mislabels states.
 */
enum class RemuxState {
    IDLE, READING_HEADERS, REMUXING, FINALIZING, DONE, ERROR, CANCELLED
}

data class RemuxProgress(
    val state: RemuxState,
    val percent: Int = 0,
    val message: String = "",
    val lastSample: TelemetrySample? = null
)

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