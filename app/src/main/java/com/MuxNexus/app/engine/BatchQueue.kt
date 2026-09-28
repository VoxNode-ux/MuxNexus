package com.offlineremux.app.engine

import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.content.edit
import org.json.JSONArray
import org.json.JSONObject

/**
 * Persists a queue of pending remux jobs (input URI -> output URI) across
 * process restarts. Relies on Android's persistable URI permission grants
 * (Storage Access Framework) rather than caching file descriptors, which
 * would not survive a process kill.
 */
data class RemuxJob(
    val id: String,
    val inputUri: Uri,
    val outputUri: Uri,
    val displayName: String,
    val format: OutputFormat = OutputFormat.MP4,
    val status: JobStatus = JobStatus.PENDING,
    val errorMessage: String? = null
)

class BatchQueue(private val context: Context) {

    private val prefs = context.getSharedPreferences("offline_remux_queue", Context.MODE_PRIVATE)

    fun takePersistableUriPermission(resolver: ContentResolver, uri: Uri, forWrite: Boolean) {
        val flags = if (forWrite) {
            Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        } else {
            Intent.FLAG_GRANT_READ_URI_PERMISSION
        }
        resolver.takePersistableUriPermission(uri, flags)
    }

    fun enqueue(job: RemuxJob) {
        val jobs = loadAll().toMutableList()
        jobs.add(job)
        saveAll(jobs)
    }

    fun remove(id: String) {
        saveAll(loadAll().filterNot { it.id == id })
    }

    /** Swaps the job at [id] with the one immediately before it in the list. */
    fun moveUp(id: String) {
        val jobs = loadAll().toMutableList()
        val index = jobs.indexOfFirst { it.id == id }
        if (index > 0) {
            val tmp = jobs[index - 1]
            jobs[index - 1] = jobs[index]
            jobs[index] = tmp
            saveAll(jobs)
        }
    }

    /** Swaps the job at [id] with the one immediately after it in the list. */
    fun moveDown(id: String) {
        val jobs = loadAll().toMutableList()
        val index = jobs.indexOfFirst { it.id == id }
        if (index in 0 until jobs.lastIndex) {
            val tmp = jobs[index + 1]
            jobs[index + 1] = jobs[index]
            jobs[index] = tmp
            saveAll(jobs)
        }
    }

    fun markRunning(id: String) = updateStatus(id, JobStatus.RUNNING, null)
    fun markDone(id: String) = updateStatus(id, JobStatus.DONE, null)
    fun markError(id: String, message: String) = updateStatus(id, JobStatus.ERROR, message)

    private fun updateStatus(id: String, status: JobStatus, errorMessage: String?) {
        val jobs = loadAll().map {
            if (it.id == id) it.copy(status = status, errorMessage = errorMessage) else it
        }
        saveAll(jobs)
    }

    /** A job left RUNNING means the process died mid-remux; make it retryable. */
    fun resetStaleRunning() {
        saveAll(loadAll().map {
            if (it.status == JobStatus.RUNNING) it.copy(status = JobStatus.PENDING) else it
        })
    }

    fun pending(): List<RemuxJob> = loadAll().filter { it.status == JobStatus.PENDING }

    fun loadAll(): List<RemuxJob> {
        val raw = prefs.getString(KEY_JOBS, null) ?: return emptyList()
        val arr = JSONArray(raw)
        return (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            RemuxJob(
                id = o.getString("id"),
                inputUri = Uri.parse(o.getString("input")),
                outputUri = Uri.parse(o.getString("output")),
                displayName = o.getString("name"),
                format = runCatching { OutputFormat.valueOf(o.optString("format", "MP4")) }
                    .getOrDefault(OutputFormat.MP4),
                status = runCatching { JobStatus.valueOf(o.optString("status", "PENDING")) }
                    .getOrDefault(JobStatus.PENDING),
                errorMessage = o.optString("error", "").ifBlank { null }
            )
        }
    }

    private fun saveAll(jobs: List<RemuxJob>) {
        val arr = JSONArray()
        jobs.forEach { j ->
            val o = JSONObject()
            o.put("id", j.id)
            o.put("input", j.inputUri.toString())
            o.put("output", j.outputUri.toString())
            o.put("name", j.displayName)
            o.put("format", j.format.name)
            o.put("status", j.status.name)
            j.errorMessage?.let { o.put("error", it) }
            arr.put(o)
        }
        prefs.edit { putString(KEY_JOBS, arr.toString()) }
    }

    companion object {
        private const val KEY_JOBS = "jobs_json"
    }
}
