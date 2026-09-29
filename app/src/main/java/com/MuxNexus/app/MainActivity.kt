package com.muxnexus.app

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContract
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.lifecycleScope
import com.offlineremux.app.engine.*
import com.offlineremux.app.ui.theme.MuxNexusTheme
import com.offlineremux.app.ui.RemuxScreen
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.util.UUID

/** Save-file picker whose MIME type can change per call (MP4 vs MOV vs MKV). */
private class CreateDocumentWithType : ActivityResultContract<Pair<String, String>, Uri?>() {
    override fun createIntent(context: Context, input: Pair<String, String>): Intent =
        Intent(Intent.ACTION_CREATE_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType(input.first)
            .putExtra(Intent.EXTRA_TITLE, input.second)

    override fun parseResult(resultCode: Int, intent: Intent?): Uri? =
        if (resultCode == android.app.Activity.RESULT_OK) intent?.data else null
}

class MainActivity : ComponentActivity() {

    private lateinit var queue: BatchQueue

    private var progressState by mutableStateOf(RemuxProgress(RemuxState.IDLE))
    private var jobs by mutableStateOf<List<RemuxJob>>(emptyList())

    private var jobHandle: Long = -1L
    private var batchCancelled = false

    // Set when the user taps "Add file" and picks a format, consumed by the pickers.
    private var pendingFormat: OutputFormat = OutputFormat.MP4
    private var pendingInputUri: Uri? = null

    private val pickInput = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        queue.takePersistableUriPermission(contentResolver, uri, forWrite = false)
        pendingInputUri = uri
        val suggested = displayName(uri).substringBeforeLast('.') + "." + pendingFormat.fileExtension
        pickOutput.launch(pendingFormat.mimeType to suggested)
    }

    private val pickOutput = registerForActivityResult(CreateDocumentWithType()) { uri ->
        val input = pendingInputUri
        pendingInputUri = null
        if (uri == null || input == null) return@registerForActivityResult
        queue.takePersistableUriPermission(contentResolver, uri, forWrite = true)
        queue.enqueue(
            RemuxJob(
                id = UUID.randomUUID().toString(),
                inputUri = input,
                outputUri = uri,
                displayName = displayName(input),
                format = pendingFormat
            )
        )
        refreshQueue()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        queue = BatchQueue(applicationContext)
        queue.resetStaleRunning()
        refreshQueue()

        lifecycleScope.launch {
            NativeBridge.telemetryFlow.collectLatest { sample ->
                progressState = progressState.copy(lastSample = sample)
            }
        }

        setContent {
            OfflineRemuxTheme {
                RemuxScreen(
                    progress = progressState,
                    queue = jobs,
                    onPickInput = { format ->
                        pendingFormat = format
                        pickInput.launch(arrayOf("video/*", "audio/*"))
                    },
                    onStart = {
                        batchCancelled = false
                        startNextJob()
                    },
                    onCancel = {
                        batchCancelled = true
                        if (jobHandle != -1L) NativeBridge.cancel(jobHandle)
                    },
                    onRemove = { id -> queue.remove(id); refreshQueue() },
                    onMoveUp = { id -> queue.moveUp(id); refreshQueue() },
                    onMoveDown = { id -> queue.moveDown(id); refreshQueue() }
                )
            }
        }
    }

    private fun refreshQueue() {
        jobs = queue.loadAll()
    }

    /** Runs the first waiting job; when it finishes, the callback starts the next one. */
    private fun startNextJob() {
        val job = queue.pending().firstOrNull()
        if (job == null) {
            progressState = RemuxProgress(RemuxState.IDLE, 0, "All files processed")
            return
        }

        val inputPfd = contentResolver.openFileDescriptor(job.inputUri, "r")
        val outputPfd = contentResolver.openFileDescriptor(job.outputUri, "w")
        if (inputPfd == null || outputPfd == null) {
            inputPfd?.close(); outputPfd?.close()
            queue.markError(job.id, "Can't open file")
            refreshQueue()
            startNextJob()
            return
        }

        queue.markRunning(job.id)
        refreshQueue()
        progressState = RemuxProgress(RemuxState.READING_HEADERS, 0, job.displayName)

        jobHandle = NativeBridge.startRemux(
            inputFd = NativeBridge.withDupedFd(inputPfd),
            outputFd = NativeBridge.withDupedFd(outputPfd),
            outputFormat = job.format.ffmpegMuxerName,
            onProgress = { state, percent, message ->
                // Called from the native worker thread; hop to the UI thread.
                runOnUiThread { onNativeProgress(job, state, percent, message) }
            }
        )
    }

    private fun onNativeProgress(job: RemuxJob, state: Int, percent: Int, message: String) {
        val remuxState = RemuxState.entries.getOrElse(state) { RemuxState.ERROR }
        progressState = progressState.copy(state = remuxState, percent = percent, message = message.ifBlank { job.displayName })

        when (remuxState) {
            RemuxState.DONE -> {
                queue.markDone(job.id)
                refreshQueue()
                if (!batchCancelled) startNextJob()
            }
            RemuxState.ERROR -> {
                queue.markError(job.id, message.ifBlank { "Failed" })
                refreshQueue()
                // A failed file shouldn't stop the rest of the batch, but a cancel should.
                if (!batchCancelled) startNextJob()
            }
            else -> Unit
        }
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val name = c.getString(0)
                if (!name.isNullOrBlank()) return name
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "file"
    }
}
