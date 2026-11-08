package com.muxnexus.app

import android.os.Handler
import android.os.Looper
import com.muxnexus.app.engine.RemuxJob

/**
 * Stable bridge from the native progress callback to the current Activity.
 * Survives Activity recreation: the Activity registers/unregisters itself
 * in onResume/onPause, so a callback during rotation goes to the new instance.
 */
object NativeCallbackRouter {
    private val handler = Handler(Looper.getMainLooper())

    @Volatile
    var listener: ((RemuxJob, Int, Int, String) -> Unit)? = null

    fun dispatch(job: RemuxJob, state: Int, percent: Int, message: String) {
        handler.post { listener?.invoke(job, state, percent, message) }
    }
}