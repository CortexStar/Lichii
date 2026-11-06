// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.GameDir
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.GameMetadata
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

object GameLibrary {
    private const val PREFS = "lichii_library"
    private const val KEY_GAMES = "games"
    private const val SCAN_DEPTH = 3

    val nativeDispatcher = Executors.newSingleThreadExecutor { r ->
        Thread(r, "GameLibrary")
    }.asCoroutineDispatcher()

    private val iconCache = ConcurrentHashMap<String, Bitmap>()
    private val noIcon = ConcurrentHashMap.newKeySet<String>()

    fun cachedGames(context: Context): List<Game> {
        val set = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getStringSet(KEY_GAMES, emptySet()) ?: emptySet()
        return set.mapNotNull {
            runCatching { Json.decodeFromString<Game>(it) }.getOrNull()
        }.map { GameOverrides.apply(it) }.sortedBy { it.title.lowercase() }
    }

    fun gameDirs(): List<GameDir> = NativeConfig.getGameDirs().toList()

    suspend fun addGameDir(uri: Uri) = withContext(nativeDispatcher) {
        val uriString = uri.toString()
        if (NativeConfig.getGameDirs().none { it.uriString == uriString }) {
            NativeConfig.addGameDir(GameDir(uriString, true))
            NativeConfig.saveGlobalConfig()
        }
    }

    suspend fun removeGameDir(uriString: String) = withContext(nativeDispatcher) {
        val dirs = NativeConfig.getGameDirs().filter { it.uriString != uriString }
        NativeConfig.setGameDirs(dirs.toTypedArray())
        NativeConfig.saveGlobalConfig()
    }

    suspend fun scan(context: Context): List<Game> = withContext(nativeDispatcher) {
        NativeLibrary.reloadKeys()
        GameMetadata.resetMetadata()
        NativeLibrary.clearFilesystemProvider()
        iconCache.clear()
        noIcon.clear()

        val games = mutableListOf<Game>()
        val validDirs = mutableListOf<GameDir>()
        for (dir in NativeConfig.getGameDirs()) {
            val uri = Uri.parse(dir.uriString)
            if (!FileUtil.isTreeUriValid(uri)) {
                Log.warning("[GameLibrary] Dropping folder without permission: ${dir.uriString}")
                continue
            }
            validDirs.add(dir)
            walk(uri, if (dir.deepScan) SCAN_DEPTH else 1, games)
        }
        NativeConfig.setGameDirs(validDirs.toTypedArray())
        NativeConfig.saveGlobalConfig()

        val sorted = games.distinctBy { it.path }.sortedBy { it.title.lowercase() }
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putStringSet(KEY_GAMES, sorted.map { Json.encodeToString(it) }.toSet())
            .apply()
        sorted
    }

    private fun walk(uri: Uri, depth: Int, out: MutableList<Game>) {
        if (depth <= 0) return
        for (file in FileUtil.listFiles(uri)) {
            if (file.isDirectory) {
                walk(file.uri, depth - 1, out)
                continue
            }
            val ext = file.filename.substringAfterLast('.', "").lowercase()
            if (ext !in Game.extensions) continue
            readGame(file.uri, file.filename)?.let { out.add(it) }
        }
    }

    private fun readGame(uri: Uri, filename: String): Game? {
        val path = uri.toString()
        return try {
            if (!GameMetadata.getIsValid(path)) {
                Log.info("[GameLibrary] Not a valid game: $filename")
                return null
            }
            val title = GameMetadata.getTitle(path).ifEmpty { filename.substringBeforeLast('.') }
            val programId = GameMetadata.getProgramId(path).ifEmpty {
                filename.substringBeforeLast('.')
            }
            GameOverrides.apply(
                Game(
                    title = title,
                    path = path,
                    programId = programId,
                    developer = GameMetadata.getDeveloper(path),
                    version = GameMetadata.getVersion(path, false),
                    isHomebrew = GameMetadata.getIsHomebrew(path)
                ),
                filename
            )
        } catch (e: Exception) {
            Log.error("[GameLibrary] Failed to read $filename: ${e.message}")
            null
        }
    }

    suspend fun icon(context: Context, game: Game): Bitmap? {
        iconCache[game.path]?.let { return it }
        if (game.path in noIcon) return null
        GameOverrides.cover(game)?.let { res ->
            return withContext(kotlinx.coroutines.Dispatchers.IO) {
                BitmapFactory.decodeResource(context.resources, res)?.also {
                    iconCache[game.path] = it
                }
            }
        }
        return withContext(nativeDispatcher) {
            val bytes = runCatching { GameMetadata.getIcon(game.path) }.getOrNull()
            val bitmap = bytes?.takeIf { it.isNotEmpty() }?.let {
                BitmapFactory.decodeByteArray(it, 0, it.size)
            }
            if (bitmap != null) iconCache[game.path] = bitmap else noIcon.add(game.path)
            bitmap
        }
    }
}
