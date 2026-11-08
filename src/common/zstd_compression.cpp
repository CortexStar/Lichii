// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <zstd.h>

#include "common/zstd_compression.h"

namespace Common::Compression {

std::vector<u8> CompressDataZSTD(const u8* source, std::size_t source_size, s32 compression_level) {
    compression_level = std::clamp(compression_level, 1, ZSTD_maxCLevel());

    const std::size_t max_compressed_size = ZSTD_compressBound(source_size);
    std::vector<u8> compressed(max_compressed_size);

    const std::size_t compressed_size =
        ZSTD_compress(compressed.data(), compressed.size(), source, source_size, compression_level);

    if (ZSTD_isError(compressed_size)) {
        // Compression failed
        return {};
    }

    compressed.resize(compressed_size);

    return compressed;
}

std::vector<u8> CompressDataZSTDDefault(const u8* source, std::size_t source_size) {
    return CompressDataZSTD(source, source_size, ZSTD_CLEVEL_DEFAULT);
}

std::vector<u8> DecompressDataZSTD(std::span<const u8> compressed) {
    return TryDecompressDataZSTD(compressed, std::vector<u8>{}.max_size())
        .value_or(std::vector<u8>{});
}

std::optional<std::vector<u8>> TryDecompressDataZSTD(std::span<const u8> compressed,
                                                     std::size_t max_size) {
    // The size comes from the frame header, so check it before allocating
    const unsigned long long decompressed_size =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || decompressed_size > max_size) {
        return std::nullopt;
    }
    std::vector<u8> decompressed(static_cast<std::size_t>(decompressed_size));

    const std::size_t uncompressed_result_size = ZSTD_decompress(
        decompressed.data(), decompressed.size(), compressed.data(), compressed.size());

    if (ZSTD_isError(uncompressed_result_size) || uncompressed_result_size != decompressed_size) {
        return std::nullopt;
    }
    return decompressed;
}

std::size_t DecompressDataZSTD(std::span<const u8> compressed, std::span<u8> output) {
    const std::size_t result =
        ZSTD_decompress(output.data(), output.size(), compressed.data(), compressed.size());
    return ZSTD_isError(result) ? 0 : result;
}

std::optional<std::vector<u8>> TryDecompressDataZSTDWithDictionary(
    std::span<const u8> compressed, std::span<const u8> dictionary, std::size_t max_size) {
    const unsigned long long size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > max_size) {
        return std::nullopt;
    }
    ZSTD_DCtx* const context = ZSTD_createDCtx();
    if (context == nullptr) {
        return std::nullopt;
    }
    std::vector<u8> out(static_cast<std::size_t>(size));
    const std::size_t result =
        ZSTD_decompress_usingDict(context, out.data(), out.size(), compressed.data(),
                                  compressed.size(), dictionary.data(), dictionary.size());
    ZSTD_freeDCtx(context);
    if (ZSTD_isError(result) || result != size) {
        return std::nullopt;
    }
    return out;
}

std::vector<u8> CompressDataZSTDWithDictionary(std::span<const u8> source,
                                               std::span<const u8> dictionary) {
    ZSTD_CCtx* const context = ZSTD_createCCtx();
    if (context == nullptr) {
        return {};
    }
    std::vector<u8> out(ZSTD_compressBound(source.size()));
    const std::size_t result =
        ZSTD_compress_usingDict(context, out.data(), out.size(), source.data(), source.size(),
                                dictionary.data(), dictionary.size(), ZSTD_CLEVEL_DEFAULT);
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(result)) {
        return {};
    }
    out.resize(result);
    return out;
}

} // namespace Common::Compression
