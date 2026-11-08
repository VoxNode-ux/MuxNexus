package com.muxnexus.app

import android.os.Handler
import android.os.Looper
import com.muxnexus.app.engine.RemuxJob

/**
 * Stable bridge from the native progress callback to the current Activity.
 * The Activity registers/unregisters itself in onResume/onPause.
 *
 * Final events (DONE / ERROR / CANCELLED) that arrive while no Activity is
 * listening (screen off, app in background) are kept and replayed as soon as
 * a listener registers, so a finished job can never stay RUNNING forever.
 * Plain progress updates are simply dropped while nobody is listening.
 * Everything here runs on the main thread.
 */
object NativeCallbackRouter {
    private class Event(val job: RemuxJob, val state: Int, val percent: Int, val message: String)

    private val handler = Handler(Looper.getMainLooper())
    private val pendingFinal = ArrayDeque<Event>()

    var listener: ((RemuxJob, Int, Int, String) -> Unit)? = null
        set(value) {
            field = value
            if (value != null) {
                while (pendingFinal.isNotEmpty()) {
                    val e = pendingFinal.removeFirst()
                    value(e.job, e.state, e.percent, e.message)
                }
            }
        }

    // RemuxState ordinals: DONE = 4, ERROR = 5, CANCELLED = 6
    private fun isFinal(state: Int) = state in 4..6

    fun dispatch(job: RemuxJob, state: Int, percent: Int, message: String) {
        handler.post {
            val l = listener
            if (l != null) l(job, state, percent, message)
            else if (isFinal(state)) pendingFinal.addLast(Event(job, state, percent, message))
        }
    }
}