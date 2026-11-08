// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Vulkan {

/// A VK_KHR_pipeline_binary key (VkPipelineBinaryKeyKHR) without the Vulkan structure around it.
struct PipelineBinaryKey {
    static constexpr size_t MaxSize = 32; ///< VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR

    u32 size{};
    std::array<u8, MaxSize> bytes{};

    bool operator==(const PipelineBinaryKey&) const = default;
};

struct PipelineBinaryKeyHash {
    size_t operator()(const PipelineBinaryKey& key) const noexcept {
        u64 value{};
        std::memcpy(&value, key.bytes.data(), sizeof(value));
        return static_cast<size_t>(value ^ key.size);
    }
};

/// One binary of a pipeline: its key and the driver's data.
struct PipelineBinaryBlob {
    PipelineBinaryKey key;
    std::vector<u8> data;
};

/// The pipeline binaries Lichii keeps on disk, by pipeline key (vkGetPipelineKeyKHR of the create
/// info). The file starts with the driver's global key, so a driver or device change starts it
/// over; each pipeline is a record appended when it was first captured, and a later record of the
/// same pipeline replaces the earlier one. Only the record offsets stay in memory: a lookup reads
/// its record from the file. Thread safe.
class PipelineBinaryStore {
public:
    static constexpr std::array<char, 8> Magic{'L', 'X', 'P', 'B', 'I', 'N', 'A', 'R'};
    static constexpr u32 Version = 1;
    /// Larger is a damaged record (an AMD graphics pipeline's binaries are tens of KiB).
    static constexpr u64 MaxDataSize = u64{64} << 20;
    static constexpr u32 MaxBinariesPerPipeline = 16;

    /// Opens `file` for the driver whose global key is `global_key`. A file of another driver,
    /// version or a damaged one starts over; a record cut off at the end (the process ended while
    /// it was written) is dropped. False when the file can't be opened or written.
    bool Open(const std::filesystem::path& file, const PipelineBinaryKey& global_key) {
        std::scoped_lock lock{mutex};
        index.clear();
        writer.close();
        reader.close();
        path = file;
        std::error_code error;
        u64 good_end = 0;
        bool reset = true;
        if (std::filesystem::is_regular_file(file, error)) {
            std::ifstream stream(file, std::ios::binary);
            if (ReadHeader(stream, global_key)) {
                reset = false;
                good_end = static_cast<u64>(stream.tellg());
                while (true) {
                    const u64 offset = static_cast<u64>(stream.tellg());
                    PipelineBinaryKey key;
                    if (!ReadRecord(stream, key, nullptr)) {
                        break;
                    }
                    index.insert_or_assign(key, offset);
                    good_end = static_cast<u64>(stream.tellg());
                }
            }
        }
        if (reset) {
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            if (!stream) {
                return false;
            }
            WriteHeader(stream, global_key);
            if (!stream) {
                return false;
            }
        } else if (std::filesystem::file_size(file, error) != good_end) {
            std::filesystem::resize_file(file, good_end, error);
            if (error) {
                return false;
            }
        }
        writer.open(file, std::ios::binary | std::ios::app);
        reader.open(file, std::ios::binary);
        return writer.is_open() && reader.is_open();
    }

    [[nodiscard]] bool IsOpen() const {
        std::scoped_lock lock{mutex};
        return writer.is_open();
    }

    [[nodiscard]] size_t Size() const {
        std::scoped_lock lock{mutex};
        return index.size();
    }

    /// The binaries stored for `pipeline`, if any.
    [[nodiscard]] std::optional<std::vector<PipelineBinaryBlob>> Find(
        const PipelineBinaryKey& pipeline) {
        std::scoped_lock lock{mutex};
        const auto it = index.find(pipeline);
        if (it == index.end() || !reader.is_open()) {
            return std::nullopt;
        }
        reader.clear();
        reader.seekg(static_cast<std::streamoff>(it->second));
        PipelineBinaryKey key;
        std::vector<PipelineBinaryBlob> blobs;
        if (!ReadRecord(reader, key, &blobs) || key != pipeline) {
            index.erase(it);
            return std::nullopt;
        }
        return blobs;
    }

    /// Appends the binaries of `pipeline` to the file; they replace an earlier record.
    bool Store(const PipelineBinaryKey& pipeline, std::span<const PipelineBinaryBlob> blobs) {
        if (blobs.empty() || blobs.size() > MaxBinariesPerPipeline) {
            return false;
        }
        std::scoped_lock lock{mutex};
        if (!writer.is_open()) {
            return false;
        }
        writer.seekp(0, std::ios::end);
        const u64 offset = static_cast<u64>(writer.tellp());
        WriteKey(writer, pipeline);
        Write(writer, static_cast<u32>(blobs.size()));
        for (const PipelineBinaryBlob& blob : blobs) {
            WriteKey(writer, blob.key);
            Write(writer, static_cast<u64>(blob.data.size()));
            writer.write(reinterpret_cast<const char*>(blob.data.data()),
                         static_cast<std::streamsize>(blob.data.size()));
        }
        writer.flush();
        if (!writer) {
            return false;
        }
        index.insert_or_assign(pipeline, offset);
        return true;
    }

private:
    template <typename T>
    static void Write(std::ostream& stream, const T& value) {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    template <typename T>
    static bool Read(std::istream& stream, T& value) {
        return static_cast<bool>(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
    }

    static void WriteKey(std::ostream& stream, const PipelineBinaryKey& key) {
        Write(stream, key.size);
        stream.write(reinterpret_cast<const char*>(key.bytes.data()),
                     static_cast<std::streamsize>(key.bytes.size()));
    }

    static bool ReadKey(std::istream& stream, PipelineBinaryKey& key) {
        return Read(stream, key.size) && key.size <= PipelineBinaryKey::MaxSize &&
               stream.read(reinterpret_cast<char*>(key.bytes.data()),
                           static_cast<std::streamsize>(key.bytes.size()));
    }

    static void WriteHeader(std::ostream& stream, const PipelineBinaryKey& global_key) {
        stream.write(Magic.data(), Magic.size());
        Write(stream, Version);
        WriteKey(stream, global_key);
    }

    static bool ReadHeader(std::istream& stream, const PipelineBinaryKey& global_key) {
        std::array<char, 8> magic{};
        u32 version{};
        PipelineBinaryKey key;
        return stream.read(magic.data(), magic.size()) && magic == Magic && Read(stream, version) &&
               version == Version && ReadKey(stream, key) && key == global_key;
    }

    /// Reads one record; with null `blobs` it skips the data.
    static bool ReadRecord(std::istream& stream, PipelineBinaryKey& key,
                           std::vector<PipelineBinaryBlob>* blobs) {
        u32 count{};
        if (!ReadKey(stream, key) || !Read(stream, count) || count == 0 ||
            count > MaxBinariesPerPipeline) {
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            PipelineBinaryBlob blob;
            u64 size{};
            if (!ReadKey(stream, blob.key) || !Read(stream, size) || size > MaxDataSize) {
                return false;
            }
            if (blobs) {
                blob.data.resize(static_cast<size_t>(size));
                if (!stream.read(reinterpret_cast<char*>(blob.data.data()),
                                 static_cast<std::streamsize>(size))) {
                    return false;
                }
                blobs->push_back(std::move(blob));
            } else {
                // Skipping past the end of the file sets no error: check the end is there.
                const auto here = stream.tellg();
                stream.seekg(0, std::ios::end);
                const auto end = stream.tellg();
                if (here < 0 || end < 0 || static_cast<u64>(end - here) < size) {
                    return false;
                }
                stream.seekg(here + static_cast<std::streamoff>(size));
            }
        }
        return static_cast<bool>(stream);
    }

    mutable std::mutex mutex;
    std::filesystem::path path;
    std::unordered_map<PipelineBinaryKey, u64, PipelineBinaryKeyHash> index;
    std::ofstream writer;
    std::ifstream reader;
};

} // namespace Vulkan
