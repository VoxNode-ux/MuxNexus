package com.muxnexus.app

import android.app.Application
import com.muxnexus.app.engine.BatchQueue

/**
 * Process-scoped setup. resetStaleRunning() belongs here, not in
 * MainActivity.onCreate — otherwise every rotation re-marks the running job
 * as PENDING and the user can accidentally double-start it.
 */
class MuxNexusApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        BatchQueue(this).resetStaleRunning()
    }
}