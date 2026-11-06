// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import androidx.annotation.DrawableRes
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game

object GameOverrides {
    private class Entry(val title: String, @DrawableRes val cover: Int)

    private val known = mapOf(
        0x0400C3F00006E000L to Entry("Mario Kart World", R.drawable.cover_mario_kart_world)
    )

    private val placeholderTitles = setOf("", "base", "update", "main", "program")

    private fun entry(game: Game): Entry? = game.programId.toLongOrNull()?.let { known[it] }

    @DrawableRes
    fun cover(game: Game): Int? = entry(game)?.cover

    fun apply(game: Game, filename: String? = null): Game {
        val title = entry(game)?.title
            ?: game.title.takeUnless { it.trim().lowercase() in placeholderTitles }
            ?: prettifyFilename(filename ?: game.path.substringAfterLast("%2F").substringAfterLast('/'))
        if (title == game.title) return game
        return Game(title, game.path, game.programId, game.developer, game.version, game.isHomebrew)
    }

    private fun prettifyFilename(name: String): String =
        android.net.Uri.decode(name)
            .substringBeforeLast('.')
            .replace('_', ' ')
            .replace(Regex("(?<=[a-z])(?=[A-Z])|(?<=[A-Za-z])(?=[0-9])"), " ")
            .trim()
            .ifEmpty { name }
}
