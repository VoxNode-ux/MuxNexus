package com.muxnexus.app

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import com.muxnexus.app.engine.RemuxJob
import com.muxnexus.app.engine.RemuxProgress
import com.muxnexus.app.engine.RemuxState

class RemuxViewModel : ViewModel() {
    var progressState by mutableStateOf(RemuxProgress(RemuxState.IDLE))
    var jobs by mutableStateOf<List<RemuxJob>>(emptyList())
    var lastError by mutableStateOf<String?>(null)
    var jobHandle: Long = -1L
    var batchCancelled: Boolean = false
}