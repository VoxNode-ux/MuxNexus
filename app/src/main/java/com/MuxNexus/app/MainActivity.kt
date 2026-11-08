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
import androidx.activity.viewModels
import androidx.lifecycle.lifecycleScope
import com.muxnexus.app.engine.*
import com.muxnexus.app.ui.theme.MuxNexusTheme
import com.muxnexus.app.ui.RemuxScreen
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.util.UUID

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
    private val vm: RemuxViewModel by viewModels()

    private var pendingFormat: OutputFormat = OutputFormat.MP4
    private var pendingInputUri: Uri? = null

    private val pickInput = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        runCatching { queue.takePersistableUriPermission(contentResolver, uri, forWrite = false) }
        pendingInputUri = uri
        val suggested = displayName(uri).substringBeforeLast('.') + "." + pendingFormat.fileExtension
        pickOutput.launch(pendingFormat.mimeType to suggested)
    }

    private val pickOutput = registerForActivityResult(CreateDocumentWithType()) { uri ->
        val input = pendingInputUri
        pendingInputUri = null
        if (uri == null || input == null) return@registerForActivityResult
        runCatching { queue.takePersistableUriPermission(contentResolver, uri, forWrite = true) }
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
        refreshQueue()

        lifecycleScope.launch {
            NativeBridge.telemetryFlow.collectLatest { sample ->
                vm.progressState = vm.progressState.copy(lastSample = sample)
            }
        }

        setContent {
            MuxNexusTheme {
                RemuxScreen(
                    progress = vm.progressState,
                    queue = vm.jobs,
                    lastError = vm.lastError,
                    isDebugBuild = BuildConfig.DEBUG,
                    onDismissError = { vm.lastError = null },
                    onPickInput = { format ->
                        pendingFormat = format
                        pickInput.launch(arrayOf("video/*", "audio/*"))
                    },
                    onStart = {
                        vm.batchCancelled = false
                        vm.lastError = null
                        startNextJob()
                    },
                    onCancel = {
                        vm.batchCancelled = true
                        if (vm.jobHandle != -1L) NativeBridge.cancel(vm.jobHandle)
                    },
                    onRemove = { id -> queue.remove(id); refreshQueue() },
                    onMoveUp = { id -> queue.moveUp(id); refreshQueue() },
                    onMoveDown = { id -> queue.moveDown(id); refreshQueue() }
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        NativeCallbackRouter.listener = ::onNativeProgress
    }

    override fun onPause() {
        NativeCallbackRouter.listener = null
        super.onPause()
    }

    private fun refreshQueue() {
        vm.jobs = queue.loadAll()
    }

    /**
     * Advances the batch. The `attempted` set guarantees termination even if
     * persistence is silently failing — each job id is tried at most once per
     * call, so no amount of state corruption can loop forever.
     */
    private fun startNextJob() {
        val attempted = mutableSetOf<String>()
        var guard = 0

        while (guard++ < 100) {
            val job = queue.pending().firstOrNull { it.id !in attempted } ?: run {
                vm.progressState = RemuxProgress(RemuxState.IDLE, 0, "All files processed")
                return
            }
            attempted += job.id

            val inputPfd = runCatching { contentResolver.openFileDescriptor(job.inputUri, "r") }.getOrNull()
            val outputPfd = runCatching { contentResolver.openFileDescriptor(job.outputUri, "w") }.getOrNull()

            if (inputPfd == null || outputPfd == null) {
                inputPfd?.close(); outputPfd?.close()
                // If persistence fails here, we still advance (job.id is in
                // `attempted`) so the loop is guaranteed to terminate.
                queue.markError(job.id, "Can't open file")
                vm.lastError = "Failed to open ${job.displayName}"
                refreshQueue()
                continue
            }

            if (!queue.markRunning(job.id)) {
                // Could not persist RUNNING: don't start the job, it would be
                // invisible to the queue on the next launch.
                inputPfd.close(); outputPfd.close()
                vm.lastError = "Storage error persisting state"
                refreshQueue()
                return
            }
            refreshQueue()
            vm.progressState = RemuxProgress(RemuxState.READING_HEADERS, 0, job.displayName)

            val inputFd = NativeBridge.detachFd(inputPfd)
            val outputFd = NativeBridge.detachFd(outputPfd)

            val handle = try {
                NativeBridge.startRemux(
                    inputFd = inputFd,
                    outputFd = outputFd,
                    outputFormat = job.format.ffmpegMuxerName,
                    onProgress = RemuxProgressCallback { state, percent, message ->
                        NativeCallbackRouter.dispatch(job, state, percent, message)
                    }
                )
            } catch (t: Throwable) {
                runCatching { android.os.ParcelFileDescriptor.adoptFd(inputFd).close() }
                runCatching { android.os.ParcelFileDescriptor.adoptFd(outputFd).close() }
                queue.markError(job.id, "Native start failed: ${t.message}")
                vm.lastError = "Failed to start ${job.displayName}"
                refreshQueue()
                continue
            }

            if (handle < 0) {
                queue.markError(job.id, "Native callback setup failed")
                vm.lastError = "Failed to start ${job.displayName}"
                refreshQueue()
                continue
            }

            vm.jobHandle = handle
            return
        }

        // Guard exhausted: stop cleanly rather than spin.
        vm.progressState = RemuxProgress(RemuxState.IDLE, 0, "Too many failures; check the queue")
        vm.lastError = "Too many consecutive failures — batch stopped"
    }

    private fun onNativeProgress(job: RemuxJob, state: Int, percent: Int, message: String) {
        val remuxState = RemuxState.entries.getOrElse(state) { RemuxState.ERROR }

        when (remuxState) {
            RemuxState.CANCELLED -> {
                vm.jobHandle = -1L
                if (vm.batchCancelled) {
                    queue.markPending(job.id)
                    vm.progressState = RemuxProgress(RemuxState.IDLE, 0, "Cancelled")
                } else {
                    queue.markError(job.id, "Cancelled unexpectedly")
                    vm.lastError = "${job.displayName}: Cancelled unexpectedly"
                }
                refreshQueue()
            }
            RemuxState.DONE -> {
                vm.jobHandle = -1L
                val verified = message.startsWith("Complete") && !message.contains("warnings")
                queue.markDone(job.id, verified)
                refreshQueue()
                vm.progressState = vm.progressState.copy(
                    state = RemuxState.DONE,
                    percent = 100,
                    message = message.ifBlank { job.displayName }
                )
                if (!vm.batchCancelled) startNextJob()
            }
            RemuxState.ERROR -> {
                vm.jobHandle = -1L
                val errMsg = message.ifBlank { "Failed" }
                queue.markError(job.id, errMsg)
                vm.lastError = "${job.displayName}: $errMsg"
                refreshQueue()
                vm.progressState = vm.progressState.copy(
                    state = RemuxState.ERROR,
                    percent = 0,
                    message = errMsg
                )
                if (!vm.batchCancelled) startNextJob()
            }
            else -> {
                vm.progressState = vm.progressState.copy(
                    state = remuxState,
                    percent = percent,
                    message = message.ifBlank { job.displayName }
                )
            }
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