// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import android.app.ActivityOptions
import android.content.Intent
import android.graphics.Bitmap
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.scaleOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.geometry.Offset
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.Log

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setLichiiContent { LichiiTheme { LibraryApp() } }
    }
}

private const val COVER_ASPECT = 5f / 7f

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LibraryApp() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var games by remember { mutableStateOf(GameLibrary.cachedGames(context)) }
    var gameDirs by remember { mutableStateOf(GameLibrary.gameDirs()) }
    var scanning by remember { mutableStateOf(false) }
    var showSettings by remember { mutableStateOf(false) }

    fun rescan() {
        if (scanning) return
        scanning = true
        scope.launch {
            games = runCatching { GameLibrary.scan(context) }
                .onFailure { Log.error("[MainActivity] Scan failed: ${it.message}") }
                .getOrDefault(games)
            gameDirs = GameLibrary.gameDirs()
            scanning = false
        }
    }

    val folderPicker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        context.contentResolver.takePersistableUriPermission(
            uri,
            Intent.FLAG_GRANT_READ_URI_PERMISSION
        )
        scope.launch {
            GameLibrary.addGameDir(uri)
            gameDirs = GameLibrary.gameDirs()
            rescan()
        }
    }

    LaunchedEffect(Unit) { if (gameDirs.isNotEmpty()) rescan() }

    val zoom = remember { Animatable(1f) }
    val curtain = remember { Animatable(0f) }
    var launched by remember { mutableStateOf(false) }
    val lifecycleOwner = androidx.compose.ui.platform.LocalLifecycleOwner.current
    androidx.compose.runtime.DisposableEffect(lifecycleOwner) {
        val observer = androidx.lifecycle.LifecycleEventObserver { _, event ->
            if (event == androidx.lifecycle.Lifecycle.Event.ON_RESUME && launched) {
                launched = false
                scope.launch {
                    zoom.snapTo(1.08f)
                    curtain.snapTo(1f)
                    launch { zoom.animateTo(1f, Motion.bouncy()) }
                    curtain.animateTo(0f, Motion.soft())
                }
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    fun launchGame(game: Game) {
        if (launched) return
        launched = true
        scope.launch {
            launch { zoom.animateTo(1.12f, Motion.fluid()) }
            curtain.animateTo(
                1f,
                androidx.compose.animation.core.spring(stiffness = androidx.compose.animation.core.Spring.StiffnessMediumLow)
            )
            val noAnim = ActivityOptions.makeCustomAnimation(context, 0, 0)
            context.startActivity(EmulationActivity.launchIntent(context, game), noAnim.toBundle())
        }
    }

    BackHandler(enabled = showSettings) { showSettings = false }

    Box(
        Modifier
            .fillMaxSize()
            .drawWithContent {
                drawContent()
                drawRect(Color.Black.copy(alpha = curtain.value.coerceIn(0f, 1f)))
            }
    ) {
    BrandBackdrop(Modifier.matchParentSize())
    Scaffold(
        modifier = Modifier.graphicsLayer {
            scaleX = zoom.value
            scaleY = zoom.value
        },
        containerColor = Color.Transparent,
        contentColor = MaterialTheme.colorScheme.onBackground,
        topBar = {
            TopAppBar(
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = Color.Transparent
                ),
                title = {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Wordmark(Modifier.height(34.dp))
                        AnimatedVisibility(
                            visible = showSettings,
                            enter = fadeIn(Motion.soft()) +
                                slideInHorizontally(Motion.bouncy()) { -it / 2 },
                            exit = fadeOut(Motion.soft()) +
                                slideOutHorizontally(Motion.soft()) { -it / 2 }
                        ) {
                            Text(
                                "  settings",
                                style = MaterialTheme.typography.titleLarge,
                                fontWeight = FontWeight.SemiBold,
                                color = MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                },
                actions = {
                    AnimatedVisibility(
                        visible = !showSettings,
                        enter = fadeIn(Motion.soft()) + scaleIn(Motion.bouncy()),
                        exit = fadeOut(Motion.soft()) + scaleOut(Motion.soft())
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Crossfade(scanning, animationSpec = Motion.soft(), label = "scan") {
                                if (it) {
                                    Box(Modifier.size(64.dp, 40.dp), Alignment.Center) {
                                        CircularProgressIndicator(
                                            Modifier.size(20.dp),
                                            strokeWidth = 2.dp
                                        )
                                    }
                                } else {
                                    PillButton("Refresh") { rescan() }
                                }
                            }
                            Spacer(Modifier.width(8.dp))
                            PillButton("Settings") { showSettings = true }
                        }
                    }
                    AnimatedVisibility(
                        visible = showSettings,
                        enter = fadeIn(Motion.soft()) + scaleIn(Motion.bouncy()),
                        exit = fadeOut(Motion.soft()) + scaleOut(Motion.soft())
                    ) {
                        PillButton("Done", filled = true) { showSettings = false }
                    }
                    Spacer(Modifier.width(8.dp))
                }
            )
        }
    ) { padding ->
        AnimatedContent(
            targetState = showSettings,
            transitionSpec = {
                val dir = if (targetState) 1 else -1
                (
                    slideInHorizontally(Motion.fluid()) { it / 3 * dir } +
                        fadeIn(Motion.soft()) +
                        scaleIn(Motion.fluid(), initialScale = 0.94f)
                    ) togetherWith (
                    slideOutHorizontally(Motion.fluid()) { -it / 4 * dir } +
                        fadeOut(Motion.soft()) +
                        scaleOut(Motion.fluid(), targetScale = 0.96f)
                    )
            },
            label = "screen",
            modifier = Modifier.padding(padding).fillMaxSize()
        ) { settings ->
            when {
                settings -> SettingsScreen(
                    gameDirs = gameDirs,
                    onAddFolder = { folderPicker.launch(null) },
                    onRemoveFolder = { uri ->
                        scope.launch {
                            GameLibrary.removeGameDir(uri)
                            gameDirs = GameLibrary.gameDirs()
                            rescan()
                        }
                    }
                )
                games.isEmpty() -> EmptyLibrary(
                    scanning = scanning,
                    hasFolders = gameDirs.isNotEmpty(),
                    onAddFolder = { folderPicker.launch(null) }
                )
                else -> LazyVerticalGrid(
                    columns = GridCells.Adaptive(140.dp),
                    contentPadding = PaddingValues(12.dp),
                    modifier = Modifier.fillMaxSize()
                ) {
                    itemsIndexed(games, key = { _, g -> g.path }) { index, game ->
                        GameCard(
                            game,
                            index,
                            Modifier.animateItem(
                                fadeInSpec = Motion.soft(),
                                placementSpec = Motion.fluid(),
                                fadeOutSpec = Motion.soft()
                            )
                        ) {
                            launchGame(game)
                        }
                    }
                }
            }
        }
    }
    }
}

@Composable
private fun Wordmark(modifier: Modifier) {
    Image(
        painterResource(R.drawable.lichii_wordmark),
        contentDescription = "Lichii",
        contentScale = ContentScale.Fit,
        modifier = modifier
    )
}

@Composable
private fun BrandBackdrop(modifier: Modifier) {
    val scheme = MaterialTheme.colorScheme
    val dark = scheme.background.luminance() < 0.3f
    val mist = if (dark) Brand.LeafDeep.copy(alpha = 0.22f) else Brand.LeafMist.copy(alpha = 0.75f)
    val blush = if (dark) Brand.Raspberry.copy(alpha = 0.10f) else Brand.Flesh.copy(alpha = 0.9f)
    androidx.compose.foundation.Canvas(modifier.background(scheme.background)) {
        drawCircle(
            Brush.radialGradient(
                listOf(mist, Color.Transparent),
                center = Offset(size.width * 1.05f, -size.height * 0.05f),
                radius = size.maxDimension * 0.75f
            ),
            radius = size.maxDimension * 0.75f,
            center = Offset(size.width * 1.05f, -size.height * 0.05f)
        )
        drawCircle(
            Brush.radialGradient(
                listOf(mist, Color.Transparent),
                center = Offset(-size.width * 0.1f, size.height * 1.05f),
                radius = size.maxDimension * 0.7f
            ),
            radius = size.maxDimension * 0.7f,
            center = Offset(-size.width * 0.1f, size.height * 1.05f)
        )
        drawCircle(
            Brush.radialGradient(
                listOf(blush, Color.Transparent),
                center = Offset(size.width * 0.5f, size.height * 0.45f),
                radius = size.minDimension * 0.8f
            ),
            radius = size.minDimension * 0.8f,
            center = Offset(size.width * 0.5f, size.height * 0.45f)
        )
    }
}

@Composable
private fun PillButton(text: String, filled: Boolean = false, onClick: () -> Unit) {
    Box(
        Modifier
            .bouncyClick(onClick = onClick)
            .then(
                if (filled) {
                    Modifier.background(Brand.gradient, RoundedCornerShape(50))
                } else {
                    Modifier.background(
                        MaterialTheme.colorScheme.surfaceContainerHigh,
                        RoundedCornerShape(50)
                    )
                }
            )
            .padding(horizontal = 14.dp, vertical = 8.dp)
    ) {
        Text(
            text,
            style = MaterialTheme.typography.labelLarge,
            color = if (filled) Color.White else MaterialTheme.colorScheme.onSurface
        )
    }
}

@Composable
private fun EmptyLibrary(
    scanning: Boolean,
    hasFolders: Boolean,
    onAddFolder: () -> Unit
) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            BobbingFruit(Modifier.size(132.dp))
            Spacer(Modifier.height(20.dp))
            Crossfade(scanning to hasFolders, animationSpec = Motion.soft(), label = "empty") {
                (isScanning, folders) ->
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    when {
                        isScanning -> {
                            CircularProgressIndicator()
                            Spacer(Modifier.height(12.dp))
                            Text("Scanning game folders…")
                        }
                        folders -> Text("No games found in your folders.")
                        else -> Text("Pick the folder that holds your games.")
                    }
                }
            }
            if (!scanning) {
                Spacer(Modifier.height(16.dp))
                PillButton("Add game folder", filled = true, onClick = onAddFolder)
                Spacer(Modifier.height(8.dp))
                Text(
                    "Switch 2 .nup and Switch .xci/.nsp/.nca/.nro/.nsz/.xcz",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

@Composable
private fun GameCard(game: Game, index: Int, modifier: Modifier, onClick: () -> Unit) {
    val context = LocalContext.current
    val icon by produceState<Bitmap?>(null, game.path) { value = GameLibrary.icon(context, game) }

    val appear = remember { Animatable(0f) }
    LaunchedEffect(Unit) {
        delay(40L * index.coerceAtMost(12))
        appear.animateTo(1f, Motion.bouncy())
    }

    Column(
        modifier
            .padding(6.dp)
            .graphicsLayer {
                val s = 0.85f + 0.15f * appear.value
                scaleX = s
                scaleY = s
                alpha = appear.value.coerceIn(0f, 1f)
                translationY = (1f - appear.value) * 24.dp.toPx()
            }
            .bouncyClick(pressedScale = 0.94f, onClick = onClick)
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .aspectRatio(COVER_ASPECT)
                .shadow(8.dp, RoundedCornerShape(14.dp), spotColor = Brand.RaspberryDeep)
                .clip(RoundedCornerShape(14.dp))
                .background(MaterialTheme.colorScheme.surfaceVariant),
            contentAlignment = Alignment.Center
        ) {
            Crossfade(icon, animationSpec = Motion.soft(), label = "cover") { bitmap ->
                if (bitmap != null) {
                    Image(
                        bitmap = bitmap.asImageBitmap(),
                        contentDescription = game.title,
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize()
                    )
                } else {
                    Box(Modifier.fillMaxSize(), Alignment.Center) {
                        Text(
                            game.title.take(1).uppercase(),
                            style = MaterialTheme.typography.displaySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                }
            }
        }
        Text(
            game.title,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(top = 8.dp, start = 2.dp, end = 2.dp)
        )
        if (game.version.isNotEmpty()) {
            Text(
                "v${game.version}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 2.dp)
            )
        }
    }
}

@Composable
private fun BobbingFruit(modifier: Modifier) {
    val pop = remember { Animatable(0f) }
    LaunchedEffect(Unit) { pop.animateTo(1f, Motion.bouncy()) }
    val bob = androidx.compose.animation.core.rememberInfiniteTransition(label = "bob")
    val t by bob.animateFloat(
        initialValue = -1f,
        targetValue = 1f,
        animationSpec = androidx.compose.animation.core.infiniteRepeatable(
            androidx.compose.animation.core.tween(
                1600,
                easing = androidx.compose.animation.core.FastOutSlowInEasing
            ),
            androidx.compose.animation.core.RepeatMode.Reverse
        ),
        label = "t"
    )
    Image(
        painterResource(R.drawable.lichii_fruit),
        contentDescription = null,
        modifier = modifier.graphicsLayer {
            scaleX = pop.value
            scaleY = pop.value
            translationY = t * 6.dp.toPx()
            rotationZ = t * 3f
        }
    )
}
