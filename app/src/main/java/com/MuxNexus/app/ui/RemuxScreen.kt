package com.muxnexus.app.ui

import androidx.compose.animation.AnimatedVisibility
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.muxnexus.app.engine.*

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun RemuxScreen(
    lastError: String?,
    onDismissError: () -> Unit,
    progress: RemuxProgress,
    queue: List<RemuxJob>,
    onPickInput: (OutputFormat) -> Unit,
    onStart: () -> Unit,
    onCancel: () -> Unit,
    onRemove: (String) -> Unit,
    onMoveUp: (String) -> Unit,
    onMoveDown: (String) -> Unit
) {
    var showFormatPicker by remember { mutableStateOf(false) }
    val isBusy = progress.state == RemuxState.REMUXING ||
        progress.state == RemuxState.READING_HEADERS ||
        progress.state == RemuxState.FINALIZING

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("MuxNexus", fontWeight = FontWeight.SemiBold)
                        Text(
                            "Offline video remuxing — no network, ever",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
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
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Spacer(Modifier.height(4.dp))
            if (lastError != null) {
                ErrorBanner(lastError, onDismissError)
            }
            StatusCard(progress)

            Row(
                horizontalArrangement = Arrangement.spacedBy(12.dp),
                modifier = Modifier.fillMaxWidth()
            ) {
                Button(
                    onClick = onStart,
                    enabled = !isBusy && queue.any { it.status == JobStatus.PENDING },
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(vertical = 14.dp)
                ) {
                    Icon(Icons.Filled.PlayArrow, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text("Start batch", fontSize = 16.sp)
                }
                OutlinedButton(
                    onClick = onCancel,
                    enabled = isBusy,
                    modifier = Modifier.weight(1f),
                    contentPadding = PaddingValues(vertical = 14.dp)
                ) {
                    Icon(Icons.Filled.Close, contentDescription = null)
                    Spacer(Modifier.width(8.dp))
                    Text("Cancel", fontSize = 16.sp)
                }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(
                    "Queue",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold
                )
                if (queue.isNotEmpty()) {
                    Text(
                        "${queue.count { it.status == JobStatus.DONE }} of ${queue.size} done",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }

            if (queue.isEmpty()) {
                EmptyQueueState(onAddFile = { showFormatPicker = true })
            } else {
                LazyColumn(
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                    contentPadding = PaddingValues(bottom = 96.dp)
                ) {
                    itemsIndexed(queue) { index, job ->
                        JobCard(
                            job = job,
                            isFirst = index == 0,
                            isLast = index == queue.lastIndex,
                            onRemove = { onRemove(job.id) },
                            onMoveUp = { onMoveUp(job.id) },
                            onMoveDown = { onMoveDown(job.id) }
                        )
                    }
                }
            }
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

/** Small helper since LazyColumn's `items` doesn't give an index by default here. */
private fun androidx.compose.foundation.lazy.LazyListScope.itemsIndexed(
    list: List<RemuxJob>,
    content: @Composable (Int, RemuxJob) -> Unit
) {
    items(list.size, key = { list[it].id }) { i -> content(i, list[i]) }
}

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
            textAlign = androidx.compose.ui.text.style.TextAlign.Center
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
                    "Choose the container to remux into. The next screen lets you pick the source file.",
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
                        Column {
                            Text(format.label, fontWeight = FontWeight.Medium)
                        }
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
private fun StatusCard(progress: RemuxProgress) {
    val (icon, tint) = stateVisual(progress.state)
    ElevatedCard(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Box(
                    modifier = Modifier
                        .size(44.dp)
                        .clip(CircleShape)
                        .background(tint.copy(alpha = 0.15f)),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(icon, contentDescription = null, tint = tint)
                }
                Column(Modifier.weight(1f)) {
                    Text(stateLabel(progress.state), style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                    if (progress.message.isNotBlank()) {
                        Text(
                            progress.message,
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
                Text("${progress.percent}%", style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            }

            LinearProgressIndicator(
                progress = { progress.percent / 100f },
                modifier = Modifier
                    .fillMaxWidth()
                    .height(8.dp)
                    .clip(RoundedCornerShape(4.dp)),
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
        modifier = Modifier.fillMaxWidth()
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
    ElevatedCard(modifier = Modifier.fillMaxWidth()) {
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
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    FormatChip(job.format.shortLabel)
                    StatusChip(job.status, job.errorMessage)
                }
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Row {
                    IconButton(onClick = onMoveUp, enabled = !isFirst) {
                        Icon(Icons.Filled.KeyboardArrowUp, contentDescription = "Move up in queue")
                    }
                    IconButton(onClick = onMoveDown, enabled = !isLast) {
                        Icon(Icons.Filled.KeyboardArrowDown, contentDescription = "Move down in queue")
                    }
                }
                IconButton(onClick = onRemove) {
                    Icon(
                        Icons.Filled.Delete,
                        contentDescription = "Remove from queue",
                        tint = MaterialTheme.colorScheme.error
                    )
                }
            }
        }
    }
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

private fun stateLabel(state: RemuxState): String = when (state) {
    RemuxState.IDLE -> "Ready"
    RemuxState.READING_HEADERS -> "Reading file headers"
    RemuxState.REMUXING -> "Remuxing"
    RemuxState.FINALIZING -> "Finishing up"
    RemuxState.DONE -> "Complete"
    RemuxState.ERROR -> "Something went wrong"
}

@Composable
private fun stateVisual(state: RemuxState): Pair<ImageVector, Color> = when (state) {
    RemuxState.IDLE -> Icons.Filled.Inventory2 to MaterialTheme.colorScheme.onSurfaceVariant
    RemuxState.READING_HEADERS -> Icons.Filled.Search to MaterialTheme.colorScheme.primary
    RemuxState.REMUXING -> Icons.Filled.Sync to MaterialTheme.colorScheme.primary
    RemuxState.FINALIZING -> Icons.Filled.Save to MaterialTheme.colorScheme.primary
    RemuxState.DONE -> Icons.Filled.CheckCircle to Color(0xFF2E7D32)
    RemuxState.ERROR -> Icons.Filled.ErrorOutline to MaterialTheme.colorScheme.error
}

private fun formatBytes(bytes: Long): String = when {
    bytes >= 1_000_000 -> "%.1f MB".format(bytes / 1_000_000.0)
    bytes >= 1_000 -> "%.1f KB".format(bytes / 1_000.0)
    else -> "$bytes B"
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
                Icon(
                    Icons.Filled.Close,
                    contentDescription = "Dismiss error",
                    tint = MaterialTheme.colorScheme.onErrorContainer
                )
            }
        }
    }
}
