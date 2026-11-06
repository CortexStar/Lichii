// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import android.view.HapticFeedbackConstants
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.PointerId
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlin.math.hypot
import org.yuzu.yuzu_emu.features.input.NativeInput
import org.yuzu.yuzu_emu.features.input.model.NativeAnalog
import org.yuzu.yuzu_emu.features.input.model.NativeButton
import org.yuzu.yuzu_emu.features.input.model.NpadStyleIndex

private sealed class Control {
    abstract fun hit(p: Offset): Boolean

    class Button(val button: NativeButton, val label: String, val rect: Rect, val round: Boolean) :
        Control() {
        override fun hit(p: Offset): Boolean = rect.inflate(rect.minDimension * 0.15f).contains(p)
    }

    class Stick(val stick: NativeAnalog, val center: Offset, val radius: Float) : Control() {
        override fun hit(p: Offset): Boolean =
            hypot(p.x - center.x, p.y - center.y) <= radius * 1.35f
    }

    class Dpad(val center: Offset, val half: Float) : Control() {
        override fun hit(p: Offset): Boolean =
            abs(p.x - center.x) <= half * 1.15f && abs(p.y - center.y) <= half * 1.15f
    }
}

private fun buildLayout(w: Float, h: Float, u: Float): List<Control> {
    fun circle(b: NativeButton, label: String, cx: Float, cy: Float, r: Float) =
        Control.Button(b, label, Rect(Offset(cx, cy), r), round = true)

    fun pill(b: NativeButton, label: String, cx: Float, cy: Float) =
        Control.Button(
            b,
            label,
            Rect(cx - 38 * u, cy - 19 * u, cx + 38 * u, cy + 19 * u),
            round = false
        )

    val abxyX = w - 115 * u
    val abxyY = h - 125 * u
    val off = 50 * u
    val r = 27 * u
    return listOf(
        Control.Stick(NativeAnalog.LStick, Offset(120 * u, h - 120 * u), 68 * u),
        Control.Dpad(Offset(255 * u, h - 62 * u), 48 * u),
        circle(NativeButton.A, "A", abxyX + off, abxyY, r),
        circle(NativeButton.B, "B", abxyX, abxyY + off, r),
        circle(NativeButton.X, "X", abxyX, abxyY - off, r),
        circle(NativeButton.Y, "Y", abxyX - off, abxyY, r),
        pill(NativeButton.ZL, "ZL", 55 * u, 32 * u),
        pill(NativeButton.L, "L", 145 * u, 32 * u),
        pill(NativeButton.R, "R", w - 145 * u, 32 * u),
        pill(NativeButton.ZR, "ZR", w - 55 * u, 32 * u),
        circle(NativeButton.Minus, "−", w / 2 - 70 * u, h - 28 * u, 18 * u),
        circle(NativeButton.Plus, "+", w / 2 + 70 * u, h - 28 * u, 18 * u)
    )
}

private val DPAD_BUTTONS = listOf(
    NativeButton.DUp,
    NativeButton.DDown,
    NativeButton.DLeft,
    NativeButton.DRight
)

@Composable
fun TouchControls(opacity: Float, scale: Float, modifier: Modifier = Modifier) {
    val density = LocalDensity.current
    val view = LocalView.current
    val textMeasurer = rememberTextMeasurer()

    val pressed = remember { mutableStateMapOf<NativeButton, Int>() }
    val stickKnob = remember { mutableStateOf(Offset.Zero) }

    BoxWithConstraints(modifier.fillMaxSize()) {
        val w = with(density) { maxWidth.toPx() }
        val h = with(density) { maxHeight.toPx() }
        val u = density.density * scale
        val controls = remember(w, h, u) { buildLayout(w, h, u) }

        val input = remember(controls) {
            TouchInputState(controls, pressed, stickKnob) {
                view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
            }
        }
        DisposableEffect(input) { onDispose { input.releaseAll() } }

        Canvas(
            Modifier
                .fillMaxSize()
                .pointerInput(input) {
                    awaitPointerEventScope {
                        while (true) {
                            val event = awaitPointerEvent()
                            for (change in event.changes) {
                                when {
                                    change.pressed && !change.previousPressed ->
                                        input.down(change.id, change.position)
                                    change.pressed ->
                                        input.move(change.id, change.position)
                                    change.previousPressed ->
                                        input.up(change.id)
                                }
                                change.consume()
                            }
                        }
                    }
                }
        ) {
            val base = Color.White.copy(alpha = opacity)
            val fill = Color.White.copy(alpha = opacity * 0.35f)
            val style = TextStyle(
                color = base,
                fontSize = (14 * scale).sp,
                fontWeight = FontWeight.Bold
            )
            for (c in controls) {
                when (c) {
                    is Control.Button -> {
                        val down = (pressed[c.button] ?: 0) > 0
                        if (c.round) {
                            drawCircle(fill.takeIf { down } ?: fill.copy(alpha = fill.alpha * 0.3f),
                                c.rect.width / 2, c.rect.center)
                            drawCircle(base, c.rect.width / 2, c.rect.center, style = Stroke(2.5f * u))
                        } else {
                            val cr = CornerRadius(c.rect.height / 2)
                            drawRoundRect(
                                fill.takeIf { down } ?: fill.copy(alpha = fill.alpha * 0.3f),
                                c.rect.topLeft, c.rect.size, cr
                            )
                            drawRoundRect(base, c.rect.topLeft, c.rect.size, cr, style = Stroke(2.5f * u))
                        }
                        drawLabel(textMeasurer, c.label, c.rect.center, style)
                    }
                    is Control.Stick -> {
                        drawCircle(base, c.radius, c.center, style = Stroke(2.5f * u))
                        val knob = c.center + stickKnob.value * c.radius
                        drawCircle(fill, c.radius * 0.45f, knob)
                        drawCircle(base, c.radius * 0.45f, knob, style = Stroke(2.5f * u))
                    }
                    is Control.Dpad -> drawDpad(c, base, fill, pressed, u, textMeasurer, style)
                }
            }
        }
    }
}

private fun DrawScope.drawLabel(
    measurer: androidx.compose.ui.text.TextMeasurer,
    text: String,
    center: Offset,
    style: TextStyle
) {
    val layout = measurer.measure(text, style)
    drawText(
        layout,
        topLeft = center - Offset(layout.size.width / 2f, layout.size.height / 2f)
    )
}

private fun DrawScope.drawDpad(
    c: Control.Dpad,
    base: Color,
    fill: Color,
    pressed: Map<NativeButton, Int>,
    u: Float,
    measurer: androidx.compose.ui.text.TextMeasurer,
    style: TextStyle
) {
    val arm = c.half * 0.62f
    val s = Size(arm, arm)
    val cells = listOf(
        NativeButton.DUp to Offset(c.center.x - arm / 2, c.center.y - c.half),
        NativeButton.DDown to Offset(c.center.x - arm / 2, c.center.y + c.half - arm),
        NativeButton.DLeft to Offset(c.center.x - c.half, c.center.y - arm / 2),
        NativeButton.DRight to Offset(c.center.x + c.half - arm, c.center.y - arm / 2)
    )
    val cr = CornerRadius(6 * u)
    for ((button, topLeft) in cells) {
        val down = (pressed[button] ?: 0) > 0
        drawRoundRect(fill.takeIf { down } ?: fill.copy(alpha = fill.alpha * 0.3f), topLeft, s, cr)
        drawRoundRect(base, topLeft, s, cr, style = Stroke(2.5f * u))
    }
    val labels = mapOf(
        NativeButton.DUp to "▲",
        NativeButton.DDown to "▼",
        NativeButton.DLeft to "◀",
        NativeButton.DRight to "▶"
    )
    val small = style.copy(fontSize = style.fontSize * 0.7f)
    for ((button, topLeft) in cells) {
        drawLabel(measurer, labels.getValue(button), topLeft + Offset(arm / 2, arm / 2), small)
    }
}

private class TouchInputState(
    val controls: List<Control>,
    val pressed: MutableMap<NativeButton, Int>,
    val stickKnob: androidx.compose.runtime.MutableState<Offset>,
    val haptic: () -> Unit
) {
    private val owner = HashMap<PointerId, Control?>()
    private val dpadState = HashMap<PointerId, Set<NativeButton>>()
    private val touchFinger = HashMap<PointerId, Int>()

    private fun port(): Int =
        if (NativeInput.getStyleIndex(0) == NpadStyleIndex.Handheld) 8 else 0

    private fun press(b: NativeButton) {
        val n = pressed[b] ?: 0
        pressed[b] = n + 1
        if (n == 0) {
            NativeInput.onOverlayButtonEvent(port(), b, NativeInput.ButtonState.PRESSED)
        }
    }

    private fun release(b: NativeButton) {
        val n = pressed[b] ?: 0
        if (n <= 1) {
            pressed.remove(b)
            if (n == 1) {
                NativeInput.onOverlayButtonEvent(port(), b, NativeInput.ButtonState.RELEASED)
            }
        } else {
            pressed[b] = n - 1
        }
    }

    fun down(id: PointerId, p: Offset) {
        val c = controls.firstOrNull { it.hit(p) }
        owner[id] = c
        when (c) {
            is Control.Button -> {
                press(c.button)
                haptic()
            }
            is Control.Stick -> updateStick(c, p)
            is Control.Dpad -> {
                updateDpad(id, c, p)
                haptic()
            }
            null -> {
                val finger = (0..9).firstOrNull { it !in touchFinger.values } ?: return
                touchFinger[id] = finger
                NativeInput.onTouchPressed(finger, p.x, p.y)
            }
        }
    }

    fun move(id: PointerId, p: Offset) {
        when (val c = owner[id]) {
            is Control.Stick -> updateStick(c, p)
            is Control.Dpad -> updateDpad(id, c, p)
            is Control.Button -> {}
            null -> touchFinger[id]?.let { NativeInput.onTouchMoved(it, p.x, p.y) }
        }
    }

    fun up(id: PointerId) {
        when (val c = owner.remove(id)) {
            is Control.Button -> release(c.button)
            is Control.Stick -> {
                stickKnob.value = Offset.Zero
                NativeInput.onOverlayJoystickEvent(port(), c.stick, 0f, 0f)
            }
            is Control.Dpad -> {
                dpadState.remove(id)?.forEach { release(it) }
            }
            null -> touchFinger.remove(id)?.let { NativeInput.onTouchReleased(it) }
        }
    }

    private fun updateStick(c: Control.Stick, p: Offset) {
        var dx = (p.x - c.center.x) / c.radius
        var dy = (p.y - c.center.y) / c.radius
        val len = hypot(dx, dy)
        if (len > 1f) {
            dx /= len
            dy /= len
        }
        stickKnob.value = Offset(dx, dy)
        NativeInput.onOverlayJoystickEvent(port(), c.stick, dx, -dy)
    }

    private fun updateDpad(id: PointerId, c: Control.Dpad, p: Offset) {
        val dx = (p.x - c.center.x) / c.half
        val dy = (p.y - c.center.y) / c.half
        val dead = 0.3f
        val now = buildSet {
            if (dy < -dead && -dy >= abs(dx) * 0.5f) add(NativeButton.DUp)
            if (dy > dead && dy >= abs(dx) * 0.5f) add(NativeButton.DDown)
            if (dx < -dead && -dx >= abs(dy) * 0.5f) add(NativeButton.DLeft)
            if (dx > dead && dx >= abs(dy) * 0.5f) add(NativeButton.DRight)
        }
        val before = dpadState[id] ?: emptySet()
        for (b in DPAD_BUTTONS) {
            if (b in now && b !in before) press(b)
            if (b !in now && b in before) release(b)
        }
        dpadState[id] = now
    }

    fun releaseAll() {
        for (id in owner.keys.toList()) up(id)
    }
}
