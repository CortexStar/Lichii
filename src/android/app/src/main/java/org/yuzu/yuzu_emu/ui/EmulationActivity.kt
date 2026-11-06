// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.disk_shader_cache.DiskShaderCacheProgress
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

class EmulationActivity : ComponentActivity(), NativeLibrary.EmulationHost, SurfaceHolder.Callback {
    private enum class State { STOPPED, RUNNING, PAUSED, STOPPING }

    private val mainHandler = Handler(Looper.getMainLooper())

    private lateinit var gamePath: String
    private lateinit var gameTitle: String

    private var state = State.STOPPED
    private var surface: android.view.Surface? = null
    private var prepared = false
    private var emulationThread: Thread? = null

    private var started by mutableStateOf(false)
    private var menuOpen by mutableStateOf(false)
    private var errorText by mutableStateOf<String?>(null)
    private var loadingText by mutableStateOf("Loading…")
    private var closing by mutableStateOf(false)

    private fun finishAnimated() {
        if (isFinishing) return
        closing = true
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        gamePath = intent.getStringExtra(EXTRA_PATH) ?: run {
            finish()
            return
        }
        gameTitle = intent.getStringExtra(EXTRA_TITLE) ?: ""

        if (android.os.Build.VERSION.SDK_INT >= 34) {
            overrideActivityTransition(OVERRIDE_TRANSITION_OPEN, 0, 0)
            overrideActivityTransition(OVERRIDE_TRANSITION_CLOSE, 0, 0)
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.attributes.layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        WindowCompat.setDecorFitsSystemWindows(window, false)
        hideSystemBars()

        NativeLibrary.emulationHost = this
        DiskShaderCacheProgress.listener = { stage, progress, max ->
            mainHandler.post {
                loadingText = when (stage) {
                    1 -> "Building shaders $progress / $max"
                    else -> "Loading…"
                }
            }
        }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (menuOpen) resumeFromMenu() else openMenu()
            }
        })

        if (NativeLibrary.isRunning()) {
            synchronized(this) { state = State.PAUSED }
            prepared = true
            started = true
            menuOpen = true
        } else {
            prepare()
        }

        setLichiiContent { LichiiTheme { EmulationScreen() } }
    }

    private fun prepare() {
        lifecycleScope.launch {
            withContext(GameLibrary.nativeDispatcher) {
                NativeConfig.reloadGlobalConfig()
                NativeLibrary.reloadKeys()
                NativeLibrary.clearFilesystemProvider()
                NativeLibrary.addFileToFilesystemProvider(gamePath)
            }
            synchronized(this@EmulationActivity) { prepared = true }
            tryStart()
        }
    }

    @Synchronized
    private fun tryStart() {
        val s = surface
        if (!prepared || state != State.STOPPED || s == null || !s.isValid) return
        Log.info("[EmulationActivity] Starting $gameTitle")
        NativeLibrary.surfaceChanged(s)
        state = State.RUNNING
        emulationThread = Thread({
            NativeLibrary.run(gamePath, 0, true)
        }, "NativeEmulation").also { it.start() }
    }

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        synchronized(this) {
            surface = holder.surface
            when (state) {
                State.STOPPED -> {}
                State.RUNNING -> NativeLibrary.surfaceChanged(holder.surface)
                State.PAUSED -> if (!menuOpen) resumeLocked()
                State.STOPPING -> {}
            }
        }
        tryStart()
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        synchronized(this) {
            if (state == State.RUNNING) {
                NativeLibrary.pauseEmulation()
                state = State.PAUSED
            }
            NativeLibrary.surfaceDestroyed()
            surface = null
        }
    }

    private fun resumeLocked() {
        val s = surface ?: return
        if (!s.isValid) return
        NativeLibrary.surfaceChanged(s)
        NativeLibrary.unpauseEmulation()
        state = State.RUNNING
    }

    private fun openMenu() {
        synchronized(this) {
            if (state == State.RUNNING) {
                NativeLibrary.pauseEmulation()
                state = State.PAUSED
            }
        }
        menuOpen = true
    }

    private fun resumeFromMenu() {
        menuOpen = false
        synchronized(this) { if (state == State.PAUSED) resumeLocked() }
    }

    private fun exitEmulation() {
        menuOpen = false
        val wasActive = synchronized(this) {
            val active = state == State.RUNNING || state == State.PAUSED
            if (active) state = State.STOPPING
            active
        }
        if (!wasActive) {
            finishAnimated()
            return
        }
        Log.info("[EmulationActivity] Stopping emulation")
        NativeLibrary.stopEmulation()
        mainHandler.postDelayed({ if (!isFinishing) finish() }, 8000)
    }

    override fun onEmulationStarted() {
        mainHandler.post { started = true }
    }

    override fun onEmulationStopped(status: Int) {
        Log.info("[EmulationActivity] Emulation stopped, status $status")
        synchronized(this) { state = State.STOPPED }
        mainHandler.post { if (errorText == null) finishAnimated() }
    }

    override fun onProgramChanged(programIndex: Int) {
        Log.warning("[EmulationActivity] Program change to $programIndex is not supported")
    }

    override fun onEmulationError(resultCode: Int) {
        synchronized(this) { state = State.STOPPED }
        val text = when (resultCode) {
            2, 7 -> "The game file could not be loaded."
            3 -> "System files (firmware) are missing."
            4 -> "The shared font is missing."
            5 -> "The Vulkan renderer failed to start."
            else -> "Emulation failed to start (code $resultCode)."
        }
        Log.error("[EmulationActivity] Start failed: code $resultCode")
        mainHandler.post { errorText = text }
    }

    override fun onResume() {
        super.onResume()
        hideSystemBars()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onDestroy() {
        if (NativeLibrary.emulationHost === this) NativeLibrary.emulationHost = null
        DiskShaderCacheProgress.listener = null
        mainHandler.removeCallbacksAndMessages(null)
        if (isFinishing) {
            val active = synchronized(this) { state == State.RUNNING || state == State.PAUSED }
            if (active) NativeLibrary.stopEmulation()
        }
        super.onDestroy()
    }

    private fun hideSystemBars() {
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    @Composable
    private fun EmulationScreen() {
        val showFps = FrontendPrefs.getBoolean(this, FrontendPrefs.SHOW_FPS)
        val showControls = FrontendPrefs.getBoolean(this, FrontendPrefs.SHOW_CONTROLS)
        val opacity = FrontendPrefs.getFloat(this, FrontendPrefs.CONTROLS_OPACITY, 0.6f)
        val scale = FrontendPrefs.getFloat(this, FrontendPrefs.CONTROLS_SCALE, 1f)

        val enter = remember { androidx.compose.animation.core.Animatable(0f) }
        val curtain = remember { androidx.compose.animation.core.Animatable(0f) }
        LaunchedEffect(Unit) { enter.animateTo(1f, Motion.bouncy()) }
        LaunchedEffect(closing) {
            if (closing) {
                curtain.animateTo(1f, Motion.soft())
                finish()
            }
        }

        Box(
            Modifier
                .fillMaxSize()
                .drawWithContent {
                    drawContent()
                    val a = curtain.value.coerceIn(0f, 1f)
                    if (a > 0f) drawRect(Color.Black.copy(alpha = a))
                }
        ) {
            AndroidView(
                factory = { ctx -> SurfaceView(ctx).also { it.holder.addCallback(this@EmulationActivity) } },
                modifier = Modifier.fillMaxSize()
            )

            androidx.compose.animation.AnimatedVisibility(
                visible = !started,
                enter = androidx.compose.animation.fadeIn(Motion.soft()),
                exit = androidx.compose.animation.fadeOut(Motion.soft()) +
                    androidx.compose.animation.scaleOut(Motion.soft(), targetScale = 1.1f),
                modifier = Modifier.align(Alignment.Center)
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier.graphicsLayer {
                        val k = 0.8f + 0.2f * enter.value
                        scaleX = k
                        scaleY = k
                        alpha = enter.value.coerceIn(0f, 1f)
                    }
                ) {
                    CircularProgressIndicator()
                    Spacer(Modifier.height(12.dp))
                    Text(gameTitle, color = Color.White)
                    Text(loadingText, color = Color.White.copy(alpha = 0.7f), fontSize = 13.sp)
                }
            }

            androidx.compose.animation.AnimatedVisibility(
                visible = started && showControls && !menuOpen,
                enter = androidx.compose.animation.fadeIn(Motion.soft()) +
                    androidx.compose.animation.scaleIn(Motion.bouncy(), initialScale = 0.92f),
                exit = androidx.compose.animation.fadeOut(Motion.soft()),
                modifier = Modifier.matchParentSize()
            ) {
                TouchControls(opacity = opacity, scale = scale)
            }

            if (started && showFps) {
                PerfText(Modifier.align(Alignment.TopStart).padding(start = 16.dp, top = 6.dp))
            }

            if (started && !menuOpen) {
                Box(
                    Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = 6.dp)
                        .size(56.dp, 36.dp)
                        .bouncyClick(pressedScale = 0.8f) { openMenu() },
                    contentAlignment = Alignment.Center
                ) { Text("❚❚", color = Color.White.copy(alpha = opacity)) }
            }

            PauseOverlay(visible = menuOpen)

            errorText?.let { text ->
                AlertDialog(
                    onDismissRequest = { finish() },
                    title = { Text("Can't start $gameTitle") },
                    text = { Text(text) },
                    confirmButton = { TextButton(onClick = { finish() }) { Text("OK") } }
                )
            }
        }
    }

    @Composable
    private fun androidx.compose.foundation.layout.BoxScope.PauseOverlay(visible: Boolean) {
        androidx.compose.animation.AnimatedVisibility(
            visible = visible,
            enter = androidx.compose.animation.fadeIn(Motion.soft()),
            exit = androidx.compose.animation.fadeOut(Motion.soft()),
            modifier = Modifier.matchParentSize()
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.55f))
                    .bouncyClick(pressedScale = 1f) { resumeFromMenu() }
            )
        }
        androidx.compose.animation.AnimatedVisibility(
            visible = visible,
            enter = androidx.compose.animation.scaleIn(Motion.bouncy(), initialScale = 0.7f) +
                androidx.compose.animation.fadeIn(Motion.soft()),
            exit = androidx.compose.animation.scaleOut(Motion.soft(), targetScale = 0.85f) +
                androidx.compose.animation.fadeOut(Motion.soft()),
            modifier = Modifier.align(Alignment.Center)
        ) {
            Column(
                Modifier
                    .background(MaterialTheme.colorScheme.surfaceContainer, RoundedCornerShape(24.dp))
                    .padding(horizontal = 28.dp, vertical = 22.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text("Paused", style = MaterialTheme.typography.titleLarge, color = MaterialTheme.colorScheme.onSurface)
                Text(
                    gameTitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(18.dp))
                androidx.compose.foundation.layout.Row(
                    horizontalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(12.dp)
                ) {
                    MenuButton("Exit", filled = false) { exitEmulation() }
                    MenuButton("Resume", filled = true) { resumeFromMenu() }
                }
            }
        }
    }

    @Composable
    private fun MenuButton(text: String, filled: Boolean, onClick: () -> Unit) {
        Box(
            Modifier
                .bouncyClick(onClick = onClick)
                .then(
                    if (filled) {
                        Modifier.background(Brand.gradient, RoundedCornerShape(50))
                    } else {
                        Modifier.background(
                            MaterialTheme.colorScheme.surfaceContainerHighest,
                            RoundedCornerShape(50)
                        )
                    }
                )
                .padding(horizontal = 26.dp, vertical = 12.dp)
        ) {
            Text(
                text,
                color = if (filled) Color.White else MaterialTheme.colorScheme.onSurface,
                style = MaterialTheme.typography.labelLarge
            )
        }
    }

    @Composable
    private fun PerfText(modifier: Modifier) {
        var text by remember { mutableStateOf("") }
        LaunchedEffect(Unit) {
            while (true) {
                val stats = NativeLibrary.getPerfStats()
                if (stats.size >= 4) {
                    val shaders = NativeLibrary.getShadersBuilding()
                    text = String.format(
                        "%.1f FPS  %.1f ms  %d%%",
                        stats[1],
                        stats[2] * 1000.0,
                        (stats[3] * 100.0).toInt()
                    ) + if (shaders > 0) "  · $shaders shaders" else ""
                }
                delay(500)
            }
        }
        Text(
            text,
            color = Color.White,
            fontSize = 12.sp,
            fontFamily = FontFamily.Monospace,
            modifier = modifier
                .background(Color.Black.copy(alpha = 0.45f), RoundedCornerShape(6.dp))
                .padding(horizontal = 6.dp, vertical = 2.dp)
        )
    }

    companion object {
        private const val EXTRA_PATH = "path"
        private const val EXTRA_TITLE = "title"

        fun launchIntent(context: Context, game: Game): Intent =
            Intent(context, EmulationActivity::class.java)
                .putExtra(EXTRA_PATH, game.path)
                .putExtra(EXTRA_TITLE, game.title)
    }
}
