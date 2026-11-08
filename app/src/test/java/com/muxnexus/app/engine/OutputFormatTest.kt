package com.muxnexus.app.engine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class OutputFormatTest {

    @Test fun `every format has non-empty mime type, extension, and muxer name`() {
        OutputFormat.entries.forEach { fmt ->
            assertTrue("label", fmt.label.isNotBlank())
            assertTrue("shortLabel", fmt.shortLabel.isNotBlank())
            assertTrue("mimeType", fmt.mimeType.isNotBlank())
            assertTrue("fileExtension", fmt.fileExtension.isNotBlank())
            assertTrue("ffmpegMuxerName", fmt.ffmpegMuxerName.isNotBlank())
        }
    }

    @Test fun `file extensions do not start with a dot`() {
        OutputFormat.entries.forEach { assertFalse(it.fileExtension, it.fileExtension.startsWith(".")) }
    }

    @Test fun `mime types use video slash prefix`() {
        OutputFormat.entries.forEach { assertTrue(it.mimeType, it.mimeType.startsWith("video/")) }
    }

    @Test fun `ffmpeg muxer names are unique`() {
        val names = OutputFormat.entries.map { it.ffmpegMuxerName }
        assertEquals(names.size, names.toSet().size)
    }

    @Test fun `extensions are unique`() {
        val exts = OutputFormat.entries.map { it.fileExtension }
        assertEquals(exts.size, exts.toSet().size)
    }

    @Test fun `mp4 format is stable for storage`() {
        assertEquals("mp4", OutputFormat.MP4.ffmpegMuxerName)
        assertEquals("MP4", OutputFormat.MP4.shortLabel)
        assertEquals("MP4", OutputFormat.MP4.name)
    }

    @Test fun `matroska is the ffmpeg muxer name, not mkv`() {
        assertEquals("matroska", OutputFormat.MKV.ffmpegMuxerName)
        assertEquals("mkv", OutputFormat.MKV.fileExtension)
    }
}