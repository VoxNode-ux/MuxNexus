package com.muxnexus.app.ui

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.material3.windowsizeclass.WindowHeightSizeClass
import androidx.compose.material3.windowsizeclass.WindowSizeClass
import androidx.compose.material3.windowsizeclass.WindowWidthSizeClass
import androidx.compose.material3.windowsizeclass.calculateWindowSizeClass
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext

/** Finds the Activity hosting a Compose tree, even inside ContextWrappers. */
internal fun Context.findActivity(): Activity? =
    generateSequence(this) { (it as? ContextWrapper)?.baseContext }
        .filterIsInstance<Activity>()
        .firstOrNull()

@Composable
internal fun rememberWindowSizeClass(): WindowSizeClass? {
    val context = LocalContext.current
    return remember(context) {
        context.findActivity()?.let { calculateWindowSizeClass(it) }
    }
}

enum class ScreenMode { CompactPortrait, CompactLandscape, Medium, Expanded }

internal fun WindowSizeClass?.screenMode(): ScreenMode = when {
    this == null -> ScreenMode.CompactPortrait
    heightSizeClass == WindowHeightSizeClass.Compact -> ScreenMode.CompactLandscape
    widthSizeClass == WindowWidthSizeClass.Compact -> ScreenMode.CompactPortrait
    widthSizeClass == WindowWidthSizeClass.Medium -> ScreenMode.Medium
    else -> ScreenMode.Expanded
}