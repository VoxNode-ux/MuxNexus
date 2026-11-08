package com.muxnexus.app

import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.compose.ui.test.performClick
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class SmokeTest {

    @get:Rule
    val rule = createAndroidComposeRule<MainActivity>()

    @Test fun appLaunchesAndShowsTitle() {
        rule.onNodeWithText("MuxNexus").assertIsDisplayed()
    }

    @Test fun appShowsEmptyQueueWhenNothingIsQueued() {
        rule.onNodeWithText("No files queued yet").assertIsDisplayed()
    }

    @Test fun addFileButtonOpensFormatPicker() {
        rule.onNodeWithText("Add your first file").performClick()
        rule.onNodeWithText("Output format").assertIsDisplayed()
        rule.onNodeWithText("MP4 (.mp4)").assertIsDisplayed()
        rule.onNodeWithText("Matroska (.mkv)").assertIsDisplayed()
    }
}
