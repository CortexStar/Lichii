// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.foundation.background
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.utils.NativeConfig

object FrontendPrefs {
    private const val PREFS = "lichii_frontend"
    const val SHOW_FPS = "show_fps"
    const val SHOW_CONTROLS = "show_controls"
    const val CONTROLS_OPACITY = "controls_opacity"
    const val CONTROLS_SCALE = "controls_scale"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun getBoolean(context: Context, key: String, default: Boolean = true) =
        prefs(context).getBoolean(key, default)

    fun setBoolean(context: Context, key: String, value: Boolean) =
        prefs(context).edit().putBoolean(key, value).apply()

    fun getFloat(context: Context, key: String, default: Float) =
        prefs(context).getFloat(key, default)

    fun setFloat(context: Context, key: String, value: Float) =
        prefs(context).edit().putFloat(key, value).apply()
}

object CoreSettings {
    const val RESOLUTION = "resolution_setup"
    const val CPU_BACKEND = "cpu_backend"
    const val DOCKED = "use_docked_mode"
    const val ASYNC_SHADERS = "use_asynchronous_shaders"
    const val DISK_SHADER_CACHE = "use_disk_shader_cache"
    const val SPEED_LIMIT = "use_speed_limit"

    val resolutions = listOf(1 to "0.5x", 2 to "0.75x", 3 to "1x", 5 to "1.5x", 6 to "2x")
    val cpuBackends = listOf(1 to "NCE", 0 to "Dynarmic")

    fun getInt(key: String) = NativeConfig.getInt(key, true)
    fun setInt(key: String, value: Int) {
        NativeConfig.setInt(key, value)
        NativeConfig.saveGlobalConfig()
    }

    fun getBoolean(key: String) = NativeConfig.getBoolean(key, true)
    fun setBoolean(key: String, value: Boolean) {
        NativeConfig.setBoolean(key, value)
        NativeConfig.saveGlobalConfig()
    }
}

@Composable
fun SettingsScreen(
    gameDirs: List<GameDir>,
    onAddFolder: () -> Unit,
    onRemoveFolder: (String) -> Unit,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    Column(
        modifier
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp)
    ) {
        SectionTitle("Graphics")
        ChoiceRow(
            title = "Resolution scale",
            options = CoreSettings.resolutions,
            initial = CoreSettings.getInt(CoreSettings.RESOLUTION)
        ) { CoreSettings.setInt(CoreSettings.RESOLUTION, it) }
        SwitchRow(
            title = "Docked mode",
            subtitle = "Off renders at handheld resolution, which is cheaper",
            initial = CoreSettings.getBoolean(CoreSettings.DOCKED)
        ) { CoreSettings.setBoolean(CoreSettings.DOCKED, it) }
        SwitchRow(
            title = "Asynchronous shaders",
            subtitle = "Less stutter while shaders compile; may draw briefly wrong",
            initial = CoreSettings.getBoolean(CoreSettings.ASYNC_SHADERS)
        ) { CoreSettings.setBoolean(CoreSettings.ASYNC_SHADERS, it) }
        SwitchRow(
            title = "Disk shader cache",
            initial = CoreSettings.getBoolean(CoreSettings.DISK_SHADER_CACHE)
        ) { CoreSettings.setBoolean(CoreSettings.DISK_SHADER_CACHE, it) }

        SectionTitle("CPU")
        ChoiceRow(
            title = "CPU backend",
            options = CoreSettings.cpuBackends,
            initial = CoreSettings.getInt(CoreSettings.CPU_BACKEND)
        ) { CoreSettings.setInt(CoreSettings.CPU_BACKEND, it) }
        SwitchRow(
            title = "Limit speed",
            subtitle = "Off lets the game run faster than real time",
            initial = CoreSettings.getBoolean(CoreSettings.SPEED_LIMIT)
        ) { CoreSettings.setBoolean(CoreSettings.SPEED_LIMIT, it) }

        SectionTitle("Overlay")
        SwitchRow(
            title = "Show FPS",
            initial = FrontendPrefs.getBoolean(context, FrontendPrefs.SHOW_FPS)
        ) { FrontendPrefs.setBoolean(context, FrontendPrefs.SHOW_FPS, it) }
        SwitchRow(
            title = "Touch controls",
            initial = FrontendPrefs.getBoolean(context, FrontendPrefs.SHOW_CONTROLS)
        ) { FrontendPrefs.setBoolean(context, FrontendPrefs.SHOW_CONTROLS, it) }
        SliderRow(
            title = "Controls opacity",
            initial = FrontendPrefs.getFloat(context, FrontendPrefs.CONTROLS_OPACITY, 0.6f),
            range = 0.2f..1f
        ) { FrontendPrefs.setFloat(context, FrontendPrefs.CONTROLS_OPACITY, it) }
        SliderRow(
            title = "Controls size",
            initial = FrontendPrefs.getFloat(context, FrontendPrefs.CONTROLS_SCALE, 1f),
            range = 0.7f..1.4f
        ) { FrontendPrefs.setFloat(context, FrontendPrefs.CONTROLS_SCALE, it) }

        SectionTitle("Game folders")
        if (gameDirs.isEmpty()) {
            Text(
                "No folders yet.",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(vertical = 8.dp)
            )
        }
        for (dir in gameDirs) {
            Row(
                Modifier.fillMaxWidth().padding(vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    prettyTreeUri(dir.uriString),
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f)
                )
                TextButton(onClick = { onRemoveFolder(dir.uriString) }) { Text("Remove") }
            }
        }
        TextButton(onClick = onAddFolder) { Text("Add folder") }
        Spacer(Modifier.padding(16.dp))
    }
}

fun prettyTreeUri(uri: String): String =
    android.net.Uri.decode(uri.substringAfterLast("/tree/", uri)).replace("primary:", "/")

@Composable
private fun SectionTitle(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(top = 20.dp, bottom = 4.dp)
    )
    HorizontalDivider()
}

@Composable
private fun SwitchRow(
    title: String,
    subtitle: String? = null,
    initial: Boolean,
    onChange: (Boolean) -> Unit
) {
    var checked by remember { mutableStateOf(initial) }
    Row(
        Modifier
            .fillMaxWidth()
            .clickable {
                checked = !checked
                onChange(checked)
            }
            .padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            if (subtitle != null) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        BouncySwitch(checked = checked, onCheckedChange = {
            checked = it
            onChange(it)
        })
    }
}

@Composable
private fun ChoiceRow(
    title: String,
    options: List<Pair<Int, String>>,
    initial: Int,
    onChange: (Int) -> Unit
) {
    var selected by remember { mutableIntStateOf(initial) }
    Column(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
        Text(title, style = MaterialTheme.typography.bodyLarge)
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.padding(top = 8.dp)
        ) {
            for ((value, label) in options) {
                BouncyChip(label = label, selected = selected == value) {
                    selected = value
                    onChange(value)
                }
            }
        }
    }
}

@Composable
private fun SliderRow(
    title: String,
    initial: Float,
    range: ClosedFloatingPointRange<Float>,
    onChange: (Float) -> Unit
) {
    var value by remember { mutableFloatStateOf(initial) }
    Row(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(title, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.width(150.dp))
        Slider(
            value = value,
            onValueChange = { value = it },
            onValueChangeFinished = { onChange(value) },
            valueRange = range,
            modifier = Modifier.weight(1f)
        )
    }
}

@Composable
private fun BouncyChip(label: String, selected: Boolean, onClick: () -> Unit) {
    val pop = remember { androidx.compose.animation.core.Animatable(1f) }
    androidx.compose.runtime.LaunchedEffect(selected) {
        if (selected) {
            pop.snapTo(0.86f)
            pop.animateTo(1f, Motion.bouncy())
        }
    }
    val bg by androidx.compose.animation.animateColorAsState(
        if (selected) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceContainerHigh, Motion.soft(), label = "chipBg"
    )
    val fg by androidx.compose.animation.animateColorAsState(
        if (selected) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
        Motion.soft(),
        label = "chipFg"
    )
    androidx.compose.foundation.layout.Box(
        Modifier
            .graphicsLayer {
                scaleX = pop.value
                scaleY = pop.value
            }
            .bouncyClick(onClick = onClick)
            .background(bg, androidx.compose.foundation.shape.RoundedCornerShape(50))
            .padding(horizontal = 14.dp, vertical = 8.dp)
    ) {
        Text(label, color = fg, style = MaterialTheme.typography.labelLarge)
    }
}
