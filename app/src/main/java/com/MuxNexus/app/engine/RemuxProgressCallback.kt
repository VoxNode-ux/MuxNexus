package com.muxnexus.app.engine

import androidx.annotation.Keep

/**
 * Stable JNI callback contract. R8 keeps this interface (see proguard-rules.pro),
 * so the native side can always resolve `onProgress(IIILjava/lang/String;)V`
 * by name — no Function3 indirection, no boxing, no rename risk.
 */
@Keep
fun interface RemuxProgressCallback {
    fun onProgress(state: Int, percent: Int, message: String)
}