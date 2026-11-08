package com.muxnexus.app.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.selection.selectable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.VideoLibrary
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.muxnexus.app.engine.*

enum class QueueFilter(val label: String) {
    ALL("All"),
    ACTIVE("Active"),
    DONE("Done"),
    FAILED("Failed");

    fun apply(jobs: List<RemuxJob>): List<RemuxJob> = when (this) {
        ALL -> jobs
        ACTIVE -> jobs.filter { it.status == JobStatus.PENDING || it.status == JobStatus.RUNNING }
        DONE -> jobs.filter { it.status == JobStatus.DONE }
        FAILED -> jobs.filter { it.status == JobStatus.ERROR }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RemuxScreen(
    lastError: String?,
    onDismissError: () -> Unit,
    progress: RemuxProgress,
    queue: List<RemuxJob>,
    isDebugBuild: Boolean,
    onPickInput: (OutputFormat) -> Unit,
    onStart: () -> Unit,
    onCancel: () -> Unit,
    onRemove: (String) -> Unit,
    onMoveUp: (String) -> Unit,
    onMoveDown: (String) -> Unit
) {
    val haptics = LocalHapticFeedback.current
    LaunchedEffect(progress.state) {
        when (progress.state) {
            RemuxState.DONE -> haptics.performHapticFeedback(HapticFeedbackType.Confirm)
            RemuxState.ERROR -> haptics.performHapticFeedback(HapticFeedbackType.Reject)
            RemuxState.CANCELLED -> haptics.performHapticFeedback(HapticFeedbackType.ContextClick)
            else -> {}
        }
    }

    val sizeClass = rememberWindowSizeClass()
    val mode = sizeClass.screenMode()

    var showFormatPicker by rememberSaveable { mutableStateOf(false) }
    var filter by rememberSaveable { mutableStateOf(QueueFilter.ALL) }

    val visibleJobs = remember(queue, filter) { filter.apply(queue) }
    val counts = remember(queue) { QueueFilter.entries.associateWith { it.apply(queue).size } }
    val busy = progress.state == RemuxState.REMUXING ||
        progress.state == RemuxState.READING_HEADERS ||
        progress.state == RemuxState.FINALIZING

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("MuxNexus", fontWeight = FontWeight.SemiBold)
                        Text(
                            if (isDebugBuild) "Debug build — offline video remuxing"
                            else "Offline video remuxing — no network, ever",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                },
                colors = if (isDebugBuild)
                    TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.errorContainer
                    )
                else TopAppBarDefaults.topAppBarColors()
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = { showFormatPicker = true },
                icon = { Icon(Icons.Filled.Add, contentDescription = null) },
                text = { Text("Add file") }
            )
        }
    ) { padding ->
        val containerModifier = Modifier
            .padding(padding)
            .fillMaxSize()

        when (mode) {
            ScreenMode.CompactPortrait -> CompactPortraitLayout(
                modifier = containerModifier,
                progress, visibleJobs, counts, filter, busy, lastError,
                onDismissError, onFilter = { filter = it },
                onStart = onStart, onCancel = onCancel,
                onRemove = onRemove, onMoveUp = onMoveUp, onMoveDown = onMoveDown,
                onAddFile = { showFormatPicker = true }
            )
            ScreenMode.CompactLandscape, ScreenMode.Medium -> TwoPaneLayout(
                modifier = containerModifier,
                progress, visibleJobs, counts, filter, busy, lastError,
                onDismissError, onFilter = { filter = it },
                onStart = onStart, onCancel = onCancel,
                onRemove = onRemove, onMoveUp = onMoveUp, onMoveDown = onMoveDown,
                onAddFile = { showFormatPicker = true }
            )
            ScreenMode.Expanded -> ExpandedLayout(
                modifier = containerModifier,
                progress, visibleJobs, counts, filter, busy, lastError,
                onDismissError, onFilter = { filter = it },
                onStart = onStart, onCancel = onCancel,
                onRemove = onRemove, onMoveUp = onMoveUp, onMoveDown = onMoveDown,
                onAddFile = { showFormatPicker = true }
            )
        }
    }

    if (showFormatPicker) {
        FormatPickerDialog(
            onDismiss = { showFormatPicker = false },
            onPick = { format ->
                showFormatPicker = false
                onPickInput(format)
            }
        )
    }
}

// ---------------------------------------------------------------- Layouts

@Composable
private fun CompactPortraitLayout(
    modifier: Modifier,
    progress: RemuxProgress,
    jobs: List<RemuxJob>,
    counts: Map<QueueFilter, Int>,
    filter: QueueFilter,
    busy: Boolean,
    lastError: String?,
    onDismissError: () -> Unit,
    onFilter: (QueueFilter) -> Unit,
    onStart: () -> Unit,
    onCancel: () -> Unit,
    onRemove: (String) -> Unit,
    onMoveUp: (String) -> Unit,
    onMoveDown: (String) -> Unit,
    onAddFile: () -> Unit
) {
    Column(
        modifier = modifier
            .padding(horizontal = 16.dp)
            .fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Spacer(Modifier.height(4.dp))
        ErrorBannerIfAny(lastError, onDismissError)
        AnimatedStateStatus(progress)
        ControlsRow(busy, progress.state, onStart, onCancel)
        FilterChipsRow(counts, filter, onFilter)
        QueueHeader(jobs)
        QueueList(
            modifier = Modifier.weight(1f),
            jobs = jobs,
            onRemove = onRemove,
            onMoveUp = onMoveUp,
            onMoveDown = onMoveDown,
            onAddFile = onAddFile
        )
    }
}

@Composable
private fun TwoPaneLayout(
    modifier: Modifier,
    progress: RemuxProgress,
    jobs: List<RemuxJob>,
    counts: Map<QueueFilter, Int>,
    filter: QueueFilter,
    busy: Boolean,
    lastError: String?,
    onDismissError: () -> Unit,
    onFilter: (QueueFilter) -> Unit,
    onStart: () -> Unit,
    onCancel: () -> Unit,
    onRemove: (String) -> Unit,
    onMoveUp: (String) -> Unit,
    onMoveDown: (String) -> Unit,
    onAddFile: () -> Unit
) {
    Row(
        modifier = modifier
            .padding(horizontal = 16.dp)
            .fillMaxSize(),
        horizontalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        Column(
            modifier = Modifier
                .weight(0.42f)
                .fillMaxHeight(),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Spacer(Modifier.height(4.dp))
            ErrorBannerIfAny(lastError, onDismissError)
            AnimatedStateStatus(progress)
            ControlsRow(busy, progress.state, onStart, onCancel)
        }
        Column(
            modifier = Modifier
                .weight(0.58f)
                .fillMaxHeight(),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Spacer(Modifier.height(4.dp))
            FilterChipsRow(counts, filter, onFilter)
            QueueHeader(jobs)
            QueueList(
                modifier = Modifier.weight(1f),
                jobs = jobs,
                onRemove = onRemove,
                onMoveUp = onMoveUp,
                onMoveDown = onMoveDown,
                onAddFile = onAddFile
            )
        }
    }
}

@Composable
private fun ExpandedLayout(
    modifier: Modifier,
    progress: RemuxProgress,
    jobs: List<RemuxJob>,
    counts: Map<QueueFilter, Int>,
    filter: QueueFilter,
    busy: Boolean,
    lastError: String?,
    onDismissError: () -> Unit,
    onFilter: (QueueFilter) -> Unit,
    onStart: () -> Unit,
    onCancel: () -> Unit,
    onRemove: (String) -> Unit,
    onMoveUp: (String) -> Unit,
    onMoveDown: (String) -> Unit,
    onAddFile: () -> Unit
) {
    Row(
        modifier = modifier
            .padding(horizontal = 24.dp)
            .fillMaxSize(),
        horizontalArrangement = Arrangement.spacedBy(24.dp)
    ) {
        Column(
            modifier = Modifier
                .weight(0.28f)
                .fillMaxHeight(),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Spacer(Modifier.height(8.dp))
            AnimatedStateStatus(progress)
            ControlsRow(busy, progress.state, onStart, onCancel)
            ErrorBannerIfAny(lastError, onDismissError)
        }
        Column(
            modifier = Modifier
                .weight(0.72f)
                .fillMaxHeight(),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Spacer(Modifier.height(8.dp))
            FilterChipsRow(counts, filter, onFilter)
            QueueHeader(jobs)
            QueueList(
                modifier = Modifier.weight(1f),
                jobs = jobs,
                onRemove = onRemove,
                onMoveUp = onMoveUp,
                onMoveDown = onMoveDown,
                onAddFile = onAddFile
            )
        }
    }
}

// ---------------------------------------------------------------- Sections

@Composable
private fun ErrorBannerIfAny(message: String?, onDismiss: () -> Unit) {
    if (message != null) ErrorBanner(message, onDismiss)
}

@Composable
private fun AnimatedStateStatus(progress: RemuxProgress) {
    AnimatedContent(
        targetState = progress.state,
        transitionSpec = { fadeIn() togetherWith fadeOut() },
        label = "status"
    ) { state ->
        StatusCard(progress.copy(state = state))
    }
}

@Composable
private fun ControlsRow(
    busy: Boolean,
    state: RemuxState,
    onStart: () -> Unit,
    onCancel: () -> Unit
) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Button(
            onClick = onStart,
            enabled = !busy,
            modifier = Modifier.weight(1f),
            contentPadding = PaddingValues(vertical = 14.dp)
        ) {
            Icon(Icons.Filled.PlayArrow, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Start batch", fontSize = 16.sp)
        }
        OutlinedButton(
            onClick = onCancel,
            enabled = busy,
            modifier = Modifier.weight(1f),
            contentPadding = PaddingValues(vertical = 14.dp)
        ) {
            Icon(Icons.Filled.Close, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Cancel", fontSize = 16.sp)
        }
    }
}

@Composable
private fun FilterChipsRow(
    counts: Map<QueueFilter, Int>,
    selected: QueueFilter,
    onSelect: (QueueFilter) -> Unit
) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        modifier = Modifier
            .fillMaxWidth()
            .semantics { contentDescription = "Queue filters" }
    ) {
        QueueFilter.entries.forEach { f ->
            val n = counts[f] ?: 0
            FilterChip(
                selected = f == selected,
                onClick = { onSelect(f) },
                label = { Text("${f.label} ($n)") },
                enabled = n > 0 || f == QueueFilter.ALL
            )
        }
    }
}

@Composable
private fun QueueHeader(jobs: List<RemuxJob>) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
        modifier = Modifier.fillMaxWidth()
    ) {
        Text("Queue", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        if (jobs.isNotEmpty()) {
            Text(
                "${jobs.count { it.status == JobStatus.DONE }} of ${jobs.size} done",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun QueueList(
    modifier: Modifier,
    jobs: List<RemuxJob>,
    onRemove: (String) -> Unit,
    onMoveUp: (String) -> Unit,
    onMoveDown: (String) -> Unit,
    onAddFile: () -> Unit
) {
    if (jobs.isEmpty()) {
        Box(
            modifier = modifier.fillMaxWidth(),
            contentAlignment = Alignment.TopCenter
        ) { EmptyQueueState(onAddFile) }
    } else {
        LazyColumn(
            modifier = modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(10.dp),
            contentPadding = PaddingValues(bottom = 96.dp)
        ) {
            items(jobs.size, key = { jobs[it].id }) { i ->
                val job = jobs[i]
                JobCard(
                    job = job,
                    isFirst = i == 0,
                    isLast = i == jobs.lastIndex,
                    onRemove = { onRemove(job.id) },
                    onMoveUp = { onMoveUp(job.id) },
                    onMoveDown = { onMoveDown(job.id) }
                )
            }
        }
    }
}

// ---------------------------------------------------------------- Card + chips

@Composable
private fun StatusCard(progress: RemuxProgress) {
    val (icon, tint) = stateVisual(progress.state)
    val label = stateLabel(progress.state)
    val sentence = buildString {
        append(label)
        append(", ")
        append(progress.percent)
        append(" percent")
        if (progress.message.isNotBlank()) { append(", "); append(progress.message) }
    }
    ElevatedCard(
        modifier = Modifier
            .fillMaxWidth()
            .semantics {
                liveRegion = LiveRegionMode.Polite
                contentDescription = sentence
            },
        elevation = CardDefaults.elevatedCardElevation(defaultElevation = 6.dp)
    ) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Box(
                    modifier = Modifier
                        .size(44.dp)
                        .clip(CircleShape)
                        .background(tint.copy(alpha = 0.15f)),
                    contentAlignment = Alignment.Center
                ) { Icon(icon, contentDescription = null, tint = tint) }
                Column(Modifier.weight(1f)) {
                    Text(label, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                    if (progress.message.isNotBlank()) {
                        Text(
                            progress.message,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                Text(
                    "${progress.percent}%",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold
                )
            }
            LinearProgressIndicator(
                progress = { progress.percent / 100f },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(8.dp)
                    .clip(RoundedCornerShape(4.dp))
                    .semantics {
                        progressBarRangeInfo = ProgressBarRangeInfo(
                            current = progress.percent.toFloat(),
                            range = 0f..100f
                        )
                    },
                color = tint
            )
            AnimatedVisibility(visible = progress.lastSample != null) {
                progress.lastSample?.let { TelemetryDetails(it) }
            }
        }
    }
}

@Composable
private fun TelemetryDetails(sample: TelemetrySample) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(10.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Text(
            "Live packet detail",
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        TelemetryRow("Stream", "#${sample.streamIndex}" + if (sample.isKeyframe) "  •  keyframe" else "")
        TelemetryRow("Timestamp", "${sample.ptsUs / 1000} ms")
        TelemetryRow("File offset", formatBytes(sample.byteOffset))
        TelemetryRow("Frame size", formatBytes(sample.frameSize.toLong()))
    }
}

@Composable
private fun TelemetryRow(label: String, value: String) {
    Row(
        horizontalArrangement = Arrangement.SpaceBetween,
        modifier = Modifier
            .fillMaxWidth()
            .semantics(mergeDescendants = true) {}
    ) {
        Text(label, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodySmall, fontWeight = FontWeight.Medium)
    }
}

@Composable
private fun JobCard(
    job: RemuxJob,
    isFirst: Boolean,
    isLast: Boolean,
    onRemove: () -> Unit,
    onMoveUp: () -> Unit,
    onMoveDown: () -> Unit
) {
    val isRunning = job.status == JobStatus.RUNNING
    ElevatedCard(
        modifier = Modifier
            .fillMaxWidth()
            .semantics(mergeDescendants = false) {
                contentDescription = "${job.displayName}, ${job.format.shortLabel}, ${statusLabel(job)}"
            },
        elevation = CardDefaults.elevatedCardElevation(defaultElevation = 2.dp)
    ) {
        Row(
            modifier = Modifier.padding(14.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(
                modifier = Modifier
                    .size(40.dp)
                    .clip(RoundedCornerShape(10.dp))
                    .background(MaterialTheme.colorScheme.primaryContainer),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    Icons.Filled.Movie,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onPrimaryContainer,
                    modifier = Modifier.size(22.dp)
                )
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(job.displayName, fontWeight = FontWeight.Medium, maxLines = 1)
                Spacer(Modifier.height(4.dp))
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    FormatChip(job.format.shortLabel)
                    StatusChip(job.status, job.errorMessage)
                    if (job.status == JobStatus.DONE) VerificationBadge(job.verified)
                }
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Row {
                    IconButton(onClick = onMoveUp, enabled = !isFirst && !isRunning) {
                        Icon(Icons.Filled.KeyboardArrowUp, contentDescription = "Move up")
                    }
                    IconButton(onClick = onMoveDown, enabled = !isLast && !isRunning) {
                        Icon(Icons.Filled.KeyboardArrowDown, contentDescription = "Move down")
                    }
                }
                IconButton(onClick = onRemove, enabled = !isRunning) {
                    Icon(
                        Icons.Filled.Delete,
                        contentDescription = "Remove",
                        tint = if (isRunning) MaterialTheme.colorScheme.outline
                               else MaterialTheme.colorScheme.error
                    )
                }
            }
        }
    }
}

@Composable
private fun VerificationBadge(verified: Boolean) {
    val (icon, tint, cd) = if (verified)
        Triple(Icons.Filled.Verified, Color(0xFF2E7D32), "Verified")
    else
        Triple(Icons.Filled.HelpOutline, MaterialTheme.colorScheme.onSurfaceVariant, "Not verified")
    Icon(icon, contentDescription = cd, tint = tint, modifier = Modifier.size(16.dp))
}

@Composable
private fun FormatChip(text: String) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .padding(horizontal = 8.dp, vertical = 3.dp)
    ) {
        Text(text, style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.SemiBold)
    }
}

@Composable
private fun StatusChip(status: JobStatus, errorMessage: String?) {
    val (label, color) = when (status) {
        JobStatus.PENDING -> "Waiting" to MaterialTheme.colorScheme.onSurfaceVariant
        JobStatus.RUNNING -> "Processing…" to MaterialTheme.colorScheme.primary
        JobStatus.DONE -> "Done" to Color(0xFF2E7D32)
        JobStatus.ERROR -> (errorMessage?.take(28) ?: "Failed") to MaterialTheme.colorScheme.error
    }
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(color.copy(alpha = 0.12f))
            .padding(horizontal = 8.dp, vertical = 3.dp)
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = color, fontWeight = FontWeight.SemiBold)
    }
}

private fun statusLabel(job: RemuxJob): String = when (job.status) {
    JobStatus.PENDING -> "Waiting"
    JobStatus.RUNNING -> "Processing"
    JobStatus.DONE -> if (job.verified) "Done, verified" else "Done"
    JobStatus.ERROR -> job.errorMessage?.take(40) ?: "Failed"
}

// ---------------------------------------------------------------- Empty state + picker

@Composable
private fun EmptyQueueState(onAddFile: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Box(
            modifier = Modifier
                .size(72.dp)
                .clip(CircleShape)
                .background(MaterialTheme.colorScheme.secondaryContainer),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                Icons.Outlined.VideoLibrary,
                contentDescription = null,
                modifier = Modifier.size(36.dp),
                tint = MaterialTheme.colorScheme.onSecondaryContainer
            )
        }
        Text("No files queued yet", style = MaterialTheme.typography.titleMedium)
        Text(
            "Add a video or audio file to remux it into a new container —\nfully offline, nothing leaves this device.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center
        )
        Spacer(Modifier.height(8.dp))
        FilledTonalButton(onClick = onAddFile) {
            Icon(Icons.Filled.Add, contentDescription = null)
            Spacer(Modifier.width(8.dp))
            Text("Add your first file")
        }
    }
}

@Composable
private fun FormatPickerDialog(onDismiss: () -> Unit, onPick: (OutputFormat) -> Unit) {
    var selected by remember { mutableStateOf(OutputFormat.MP4) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Output format") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    "Choose the container. The next screen lets you pick the source file.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = 8.dp)
                )
                OutputFormat.entries.forEach { format ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(12.dp))
                            .then(
                                if (selected == format)
                                    Modifier.background(MaterialTheme.colorScheme.secondaryContainer)
                                else Modifier
                            )
                            .selectable(selected = selected == format) { selected = format }
                            .padding(vertical = 12.dp, horizontal = 12.dp)
                    ) {
                        RadioButton(selected = selected == format, onClick = { selected = format })
                        Spacer(Modifier.width(8.dp))
                        Text(format.label, fontWeight = FontWeight.Medium)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onPick(selected) }) { Text("Choose file…") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        }
    )
}

@Composable
private fun ErrorBanner(message: String, onDismiss: () -> Unit) {
    Surface(
        color = MaterialTheme.colorScheme.errorContainer,
        shape = RoundedCornerShape(12.dp),
        modifier = Modifier.fillMaxWidth()
    ) {
        Row(
            modifier = Modifier.padding(start = 16.dp, end = 4.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                message,
                color = MaterialTheme.colorScheme.onErrorContainer,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f)
            )
            IconButton(onClick = onDismiss) {
                Icon(Icons.Filled.Close, contentDescription = "Dismiss", tint = MaterialTheme.colorScheme.onErrorContainer)
            }
        }
    }
}

// ---------------------------------------------------------------- Small helpers

private fun stateLabel(state: RemuxState): String = when (state) {
    RemuxState.IDLE -> "Ready"
    RemuxState.READING_HEADERS -> "Reading file headers"
    RemuxState.REMUXING -> "Remuxing"
    RemuxState.FINALIZING -> "Finishing up"
    RemuxState.DONE -> "Complete"
    RemuxState.ERROR -> "Something went wrong"
    RemuxState.CANCELLED -> "Cancelled"
}

@Composable
private fun stateVisual(state: RemuxState): Pair<ImageVector, Color> = when (state) {
    RemuxState.IDLE -> Icons.Filled.Inventory2 to MaterialTheme.colorScheme.onSurfaceVariant
    RemuxState.READING_HEADERS -> Icons.Filled.Search to MaterialTheme.colorScheme.primary
    RemuxState.REMUXING -> Icons.Filled.Sync to MaterialTheme.colorScheme.primary
    RemuxState.FINALIZING -> Icons.Filled.Save to MaterialTheme.colorScheme.primary
    RemuxState.DONE -> Icons.Filled.CheckCircle to Color(0xFF2E7D32)
    RemuxState.ERROR -> Icons.Filled.ErrorOutline to MaterialTheme.colorScheme.error
    RemuxState.CANCELLED -> Icons.Filled.Cancel to MaterialTheme.colorScheme.onSurfaceVariant
}

private fun formatBytes(bytes: Long): String = when {
    bytes >= 1_000_000 -> "%.1f MB".format(bytes / 1_000_000.0)
    bytes >= 1_000 -> "%.1f KB".format(bytes / 1_000.0)
    else -> "$bytes B"
}