package com.muxnexus.app.engine

import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.core.content.edit
import org.json.JSONArray
import org.json.JSONObject

data class RemuxJob(
    val id: String,
    val inputUri: Uri,
    val outputUri: Uri,
    val displayName: String,
    val format: OutputFormat = OutputFormat.MP4,
    val status: JobStatus = JobStatus.PENDING,
    val errorMessage: String? = null,
    val verified: Boolean = false
)

/**
 * Persists the queue. Every write uses commit() (synchronous) because a lost
 * status transition — e.g. a cancel that got dropped in the async apply()
 * queue — leaves the UI believing a job is still RUNNING forever. That is
 * exactly the "stuck in a corrupted loop" failure mode we refuse to allow.
 *
 * All mutators return true on success. Callers must treat a false return as
 * fatal for that job: mark it ERROR in memory and move on; do NOT keep it in
 * the pending list.
 */
class BatchQueue(private val context: Context) {

    private val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun takePersistableUriPermission(resolver: ContentResolver, uri: Uri, forWrite: Boolean) {
        val flags = if (forWrite) {
            Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        } else {
            Intent.FLAG_GRANT_READ_URI_PERMISSION
        }
        resolver.takePersistableUriPermission(uri, flags)
    }

    fun enqueue(job: RemuxJob): Boolean {
        val jobs = loadAll().toMutableList()
        jobs.add(job)
        return saveAll(jobs)
    }

    fun remove(id: String): Boolean = saveAll(loadAll().filterNot { it.id == id })

    fun moveUp(id: String): Boolean {
        val jobs = loadAll().toMutableList()
        val index = jobs.indexOfFirst { it.id == id }
        if (index <= 0) return true
        val tmp = jobs[index - 1]
        jobs[index - 1] = jobs[index]
        jobs[index] = tmp
        return saveAll(jobs)
    }

    fun moveDown(id: String): Boolean {
        val jobs = loadAll().toMutableList()
        val index = jobs.indexOfFirst { it.id == id }
        if (index !in 0 until jobs.lastIndex) return true
        val tmp = jobs[index + 1]
        jobs[index + 1] = jobs[index]
        jobs[index] = tmp
        return saveAll(jobs)
    }

    fun markRunning(id: String): Boolean = updateStatus(id, JobStatus.RUNNING, null, false)
    fun markPending(id: String): Boolean = updateStatus(id, JobStatus.PENDING, null, false)
    fun markDone(id: String, verified: Boolean): Boolean =
        updateStatus(id, JobStatus.DONE, null, verified)
    fun markError(id: String, message: String): Boolean =
        updateStatus(id, JobStatus.ERROR, message, false)

    private fun updateStatus(id: String, status: JobStatus,
                             errorMessage: String?, verified: Boolean): Boolean {
        val jobs = loadAll().map {
            if (it.id == id) it.copy(status = status, errorMessage = errorMessage, verified = verified) else it
        }
        return saveAll(jobs)
    }

    fun resetStaleRunning(): Boolean {
        val jobs = loadAll().map {
            if (it.status == JobStatus.RUNNING) it.copy(status = JobStatus.PENDING) else it
        }
        return saveAll(jobs)
    }

    fun pending(): List<RemuxJob> = loadAll().filter { it.status == JobStatus.PENDING }

    fun loadAll(): List<RemuxJob> {
        val raw = prefs.getString(KEY_JOBS, null) ?: return emptyList()
        return runCatching {
            val arr = JSONArray(raw)
            (0 until arr.length()).map { i ->
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
                    errorMessage = o.optString("error", "").ifBlank { null },
                    verified = o.optBoolean("verified", false)
                )
            }
        }.getOrElse { e ->
            // Corrupt queue file: do not throw, do not spin. Wipe and start fresh.
            Log.w("BatchQueue", "Queue JSON corrupt; resetting", e)
            prefs.edit(commit = true) { remove(KEY_JOBS) }
            emptyList()
        }
    }

    private fun saveAll(jobs: List<RemuxJob>): Boolean {
        val arr = JSONArray()
        jobs.forEach { j ->
            val o = JSONObject()
            o.put("id", j.id)
            o.put("input", j.inputUri.toString())
            o.put("output", j.outputUri.toString())
            o.put("name", j.displayName)
            o.put("format", j.format.name)
            o.put("status", j.status.name)
            o.put("verified", j.verified)
            j.errorMessage?.let { o.put("error", it) }
            arr.put(o)
        }
        // commit=true -> synchronous, durability guaranteed before returning.
        // apply() is fast but can lose the write on process death; for a
        // queue that would leave a job permanently RUNNING.
        return prefs.edit(commit = true) { putString(KEY_JOBS, arr.toString()) }
    }

    companion object {
        private const val PREFS = "offline_remux_queue"
        private const val KEY_JOBS = "jobs_json"
    }
}
