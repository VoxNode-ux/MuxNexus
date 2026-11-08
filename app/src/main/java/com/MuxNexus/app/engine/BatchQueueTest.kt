package com.muxnexus.app.engine

import android.content.Context
import android.net.Uri
import androidx.test.core.app.ApplicationProvider
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class BatchQueueTest {

    private lateinit var context: Context
    private lateinit var queue: BatchQueue

    private fun job(id: String, name: String = id) = RemuxJob(
        id = id,
        inputUri = Uri.parse("content://test/input/$id"),
        outputUri = Uri.parse("content://test/output/$id"),
        displayName = name,
        format = OutputFormat.MP4,
        status = JobStatus.PENDING,
        errorMessage = null
    )

    @Before
    fun setup() {
        context = ApplicationProvider.getApplicationContext()
        context.getSharedPreferences("offline_remux_queue", Context.MODE_PRIVATE)
            .edit().clear().commit()
        queue = BatchQueue(context)
    }

    @After
    fun teardown() {
        context.getSharedPreferences("offline_remux_queue", Context.MODE_PRIVATE)
            .edit().clear().commit()
    }

    @Test fun `empty queue returns empty list`() {
        assertTrue(queue.loadAll().isEmpty())
        assertTrue(queue.pending().isEmpty())
    }

    @Test fun `enqueue persists and loadAll returns the job`() {
        queue.enqueue(job("a"))
        val loaded = queue.loadAll()
        assertEquals(1, loaded.size)
        assertEquals("a", loaded[0].id)
        assertEquals(JobStatus.PENDING, loaded[0].status)
    }

    @Test fun `enqueue preserves insertion order`() {
        queue.enqueue(job("a")); queue.enqueue(job("b")); queue.enqueue(job("c"))
        assertEquals(listOf("a", "b", "c"), queue.loadAll().map { it.id })
    }

    @Test fun `remove deletes the correct job`() {
        queue.enqueue(job("a")); queue.enqueue(job("b"))
        queue.remove("a")
        assertEquals(listOf("b"), queue.loadAll().map { it.id })
    }

    @Test fun `remove of unknown id is a no-op`() {
        queue.enqueue(job("a"))
        queue.remove("nope")
        assertEquals(listOf("a"), queue.loadAll().map { it.id })
    }

    @Test fun `moveUp swaps with previous`() {
        queue.enqueue(job("a")); queue.enqueue(job("b")); queue.enqueue(job("c"))
        queue.moveUp("c")
        assertEquals(listOf("a", "c", "b"), queue.loadAll().map { it.id })
    }

    @Test fun `moveUp on first is a no-op`() {
        queue.enqueue(job("a")); queue.enqueue(job("b"))
        queue.moveUp("a")
        assertEquals(listOf("a", "b"), queue.loadAll().map { it.id })
    }

    @Test fun `moveDown swaps with next`() {
        queue.enqueue(job("a")); queue.enqueue(job("b")); queue.enqueue(job("c"))
        queue.moveDown("a")
        assertEquals(listOf("b", "a", "c"), queue.loadAll().map { it.id })
    }

    @Test fun `moveDown on last is a no-op`() {
        queue.enqueue(job("a")); queue.enqueue(job("b"))
        queue.moveDown("b")
        assertEquals(listOf("a", "b"), queue.loadAll().map { it.id })
    }

    @Test fun `markRunning sets status`() {
        queue.enqueue(job("a")); queue.markRunning("a")
        assertEquals(JobStatus.RUNNING, queue.loadAll().first().status)
    }

    @Test fun `markError stores the message`() {
        queue.enqueue(job("a")); queue.markError("a", "disk full")
        val j = queue.loadAll().first()
        assertEquals(JobStatus.ERROR, j.status)
        assertEquals("disk full", j.errorMessage)
    }

    @Test fun `markDone clears error message`() {
        queue.enqueue(job("a")); queue.markError("a", "temp"); queue.markDone("a")
        val j = queue.loadAll().first()
        assertEquals(JobStatus.DONE, j.status)
        assertNull(j.errorMessage)
    }

    @Test fun `resetStaleRunning turns RUNNING back to PENDING`() {
        queue.enqueue(job("a")); queue.enqueue(job("b"))
        queue.markRunning("a"); queue.markDone("b")
        queue.resetStaleRunning()
        val byId = queue.loadAll().associateBy { it.id }
        assertEquals(JobStatus.PENDING, byId["a"]!!.status)
        assertEquals(JobStatus.DONE, byId["b"]!!.status)
    }

    @Test fun `pending filters out done and error jobs`() {
        queue.enqueue(job("a")); queue.enqueue(job("b")); queue.enqueue(job("c"))
        queue.markDone("a"); queue.markError("b", "x")
        assertEquals(listOf("c"), queue.pending().map { it.id })
    }

    @Test fun `jobs survive a new BatchQueue instance`() {
        queue.enqueue(job("a"))
        assertEquals(listOf("a"), BatchQueue(context).loadAll().map { it.id })
    }

    @Test fun `corrupt JSON returns empty list instead of throwing`() {
        context.getSharedPreferences("offline_remux_queue", Context.MODE_PRIVATE)
            .edit().putString("jobs_json", "{not valid json").commit()
        assertTrue(queue.loadAll().isEmpty())
    }

    @Test fun `unknown format name falls back to MP4`() {
        context.getSharedPreferences("offline_remux_queue", Context.MODE_PRIVATE)
            .edit()
            .putString("jobs_json",
                """[{"id":"a","input":"content://test/i","output":"content://test/o","name":"a","format":"NEVER_HEARD_OF_IT","status":"PENDING"}]""")
            .commit()
        assertEquals(OutputFormat.MP4, queue.loadAll().first().format)
    }

    @Test fun `unknown status name falls back to PENDING`() {
        context.getSharedPreferences("offline_remux_queue", Context.MODE_PRIVATE)
            .edit()
            .putString("jobs_json",
                """[{"id":"a","input":"content://test/i","output":"content://test/o","name":"a","format":"MP4","status":"GONE"}]""")
            .commit()
        assertEquals(JobStatus.PENDING, queue.loadAll().first().status)
    }
}