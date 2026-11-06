// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.ui.graphics.Brush
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

object Brand {
    val Raspberry = Color(0xFFFF0658)
    val RaspberryDeep = Color(0xFFD80049)
    val Coral = Color(0xFFFF5637)
    val Pink = Color(0xFFFF557F)
    val Flesh = Color(0xFFFCE6DC)
    val Leaf = Color(0xFF91C402)
    val LeafDeep = Color(0xFF4F7A00)
    val LeafMist = Color(0xFFD6E4A0)
    val Cream = Color(0xFFFAEEDA)

    val gradient = Brush.linearGradient(listOf(Coral, Raspberry))
}

private val lightScheme = lightColorScheme(
    primary = Brand.RaspberryDeep,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFFFD9E1),
    onPrimaryContainer = Color(0xFF5C0020),
    inversePrimary = Brand.Pink,
    secondary = Brand.LeafDeep,
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFE6F2B8),
    onSecondaryContainer = Color(0xFF253A00),
    tertiary = Brand.Coral,
    onTertiary = Color.White,
    tertiaryContainer = Color(0xFFFFDBD1),
    onTertiaryContainer = Color(0xFF5A1300),
    background = Color(0xFFFBF4E8),
    onBackground = Color(0xFF2B1B1F),
    surface = Color(0xFFFBF4E8),
    onSurface = Color(0xFF2B1B1F),
    surfaceVariant = Color(0xFFF1E3D2),
    onSurfaceVariant = Color(0xFF6F5D58),
    surfaceTint = Brand.Raspberry,
    inverseSurface = Color(0xFF3A2A2C),
    inverseOnSurface = Color(0xFFFBEEE8),
    error = Color(0xFFBA1A1A),
    onError = Color.White,
    errorContainer = Color(0xFFFFDAD6),
    onErrorContainer = Color(0xFF410002),
    outline = Color(0xFFD5C3B5),
    outlineVariant = Color(0xFFEADBCB),
    scrim = Color.Black,
    surfaceBright = Color(0xFFFFFCF7),
    surfaceDim = Color(0xFFEDE2D2),
    surfaceContainerLowest = Color.White,
    surfaceContainerLow = Color(0xFFFFF9F1),
    surfaceContainer = Color(0xFFFFF5EA),
    surfaceContainerHigh = Color(0xFFF6EBDD),
    surfaceContainerHighest = Color(0xFFEFE2D1)
)

private val darkScheme = darkColorScheme(
    primary = Color(0xFFFF4D80),
    onPrimary = Color.White,
    primaryContainer = Color(0xFF6A0E2E),
    onPrimaryContainer = Color(0xFFFFD9E1),
    inversePrimary = Brand.RaspberryDeep,
    secondary = Color(0xFFB5DC4A),
    onSecondary = Color(0xFF1F3000),
    secondaryContainer = Color(0xFF33480A),
    onSecondaryContainer = Color(0xFFD9F08F),
    tertiary = Color(0xFFFF8A6B),
    onTertiary = Color(0xFF4A1000),
    tertiaryContainer = Color(0xFF6E2410),
    onTertiaryContainer = Color(0xFFFFDBD1),
    background = Color(0xFF16100F),
    onBackground = Color(0xFFF3E6E1),
    surface = Color(0xFF16100F),
    onSurface = Color(0xFFF3E6E1),
    surfaceVariant = Color(0xFF2A201E),
    onSurfaceVariant = Color(0xFFBFAEA8),
    surfaceTint = Brand.Raspberry,
    inverseSurface = Color(0xFFF3E6E1),
    inverseOnSurface = Color(0xFF2B1B1F),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF690005),
    errorContainer = Color(0xFF93000A),
    onErrorContainer = Color(0xFFFFDAD6),
    outline = Color(0xFF54443F),
    outlineVariant = Color(0xFF3A2E2B),
    scrim = Color.Black,
    surfaceBright = Color(0xFF3C302D),
    surfaceDim = Color(0xFF16100F),
    surfaceContainerLowest = Color(0xFF110B0A),
    surfaceContainerLow = Color(0xFF1E1715),
    surfaceContainer = Color(0xFF231B19),
    surfaceContainerHigh = Color(0xFF2D2422),
    surfaceContainerHighest = Color(0xFF382E2B)
)

@Composable
fun LichiiTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = if (dark) darkScheme else lightScheme, content = content)
}

object Motion {
    fun <T> bouncy() = spring<T>(dampingRatio = 0.45f, stiffness = Spring.StiffnessMediumLow)

    fun <T> fluid() = spring<T>(dampingRatio = 0.72f, stiffness = Spring.StiffnessLow)

    fun <T> soft() = spring<T>(dampingRatio = Spring.DampingRatioNoBouncy, stiffness = Spring.StiffnessMediumLow)
}

fun Modifier.bouncyClick(
    pressedScale: Float = 0.92f,
    enabled: Boolean = true,
    onClick: () -> Unit
): Modifier = composed {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val scale by animateFloatAsState(
        if (pressed) pressedScale else 1f,
        Motion.bouncy(),
        label = "press"
    )
    this
        .graphicsLayer {
            scaleX = scale
            scaleY = scale
        }
        .clickable(
            interactionSource = interaction,
            indication = null,
            enabled = enabled,
            onClick = onClick
        )
}

@Composable
fun BouncySwitch(checked: Boolean, onCheckedChange: (Boolean) -> Unit, modifier: Modifier = Modifier) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()

    val trackWidth = 52.dp
    val thumbSize = 24.dp
    val pad = 4.dp
    val travel = trackWidth - thumbSize - pad * 2

    val x by animateDpAsState(if (checked) travel else 0.dp, Motion.bouncy(), label = "thumbX")
    val stretch by animateFloatAsState(if (pressed) 1.25f else 1f, Motion.bouncy(), label = "stretch")
    val thumbScale by animateFloatAsState(if (checked) 1f else 0.8f, Motion.bouncy(), label = "thumbScale")
    val colors = MaterialTheme.colorScheme
    val track by animateColorAsState(
        if (checked) colors.primary else colors.surfaceContainerHighest,
        Motion.soft(),
        label = "track"
    )
    val thumb by animateColorAsState(
        if (checked) colors.onPrimary else colors.onSurfaceVariant,
        Motion.soft(),
        label = "thumb"
    )
    val outline by animateColorAsState(
        if (checked) colors.primary else colors.outline,
        Motion.soft(),
        label = "outline"
    )

    Box(
        modifier
            .size(trackWidth, thumbSize + pad * 2)
            .background(track, CircleShape)
            .border(1.5.dp, outline, CircleShape)
            .clickable(interactionSource = interaction, indication = null) {
                onCheckedChange(!checked)
            }
            .padding(pad),
        contentAlignment = Alignment.CenterStart
    ) {
        Box(
            Modifier
                .offset { IntOffset(x.roundToPx(), 0) }
                .size(thumbSize)
                .graphicsLayer {
                    scaleX = thumbScale * stretch
                    scaleY = thumbScale / (1f + (stretch - 1f) * 0.5f)
                    transformOrigin = androidx.compose.ui.graphics.TransformOrigin(
                        if (checked) 1f else 0f,
                        0.5f
                    )
                }
                .background(thumb, CircleShape)
        )
    }
}

private object FullMotionScale : androidx.compose.ui.MotionDurationScale {
    override val scaleFactor: Float get() = 1f
}

fun androidx.activity.ComponentActivity.setLichiiContent(content: @Composable () -> Unit) {
    val context = androidx.compose.ui.platform.AndroidUiDispatcher.CurrentThread + FullMotionScale
    val recomposer = androidx.compose.runtime.Recomposer(context)
    val scope = kotlinx.coroutines.CoroutineScope(context)
    scope.launch { recomposer.runRecomposeAndApplyChanges() }
    lifecycle.addObserver(object : androidx.lifecycle.DefaultLifecycleObserver {
        override fun onDestroy(owner: androidx.lifecycle.LifecycleOwner) {
            recomposer.cancel()
            scope.cancel()
        }
    })
    val view = androidx.compose.ui.platform.ComposeView(this).apply {
        setParentCompositionContext(recomposer)
        setContent(content)
    }
    setContentView(view)
}
