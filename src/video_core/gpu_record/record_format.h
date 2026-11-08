// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "common/common_types.h"

// GPU command-stream recordings (gpu_record/recorder.h writes them, gpu_record/replayer.h plays
// them back). The design follows Dolphin's FIFO player (DFF files): the state the GPU had when
// the recording started, then everything the GPU emulation consumed after that, in the order
// the guest handed it over, so a replay feeds the same input to the real GPU emulation without
// a guest CPU.
//
// File: the header (magic, version, metadata text), then records of
//   u32 type, u32 flags (bit 0: zstd), u64 stored size, u64 raw size, payload.
// Every integer is little-endian. Records keep their meaning across builds: register arrays are
// the hardware's, framebuffers and command lists are written field by field.
namespace VideoCommon::GpuRecord {

inline constexpr std::array<char, 8> FileMagic{'L', 'X', 'G', 'P', 'U', 'R', 'E', 'C'};
inline constexpr u32 FormatVersion = 1;
inline constexpr u64 PageBits = 12;
inline constexpr u64 PageSize = u64{1} << PageBits;

enum class EventType : u32 {
    PhysPages = 1,           ///< Contents of emulated physical pages
    SmmuMap = 2,             ///< Device pages mapped to physical pages
    SmmuUnmap = 3,           ///< Device range unmapped
    ZeroReadRange = 4,       ///< Device range that reads zero where nothing backs it
    AddressSpace = 5,        ///< A GPU address space (Tegra::MemoryManager)
    GmmuOps = 6,             ///< GPU page table changes, at once or in GPU order
    Channel = 7,             ///< A GPU channel bound to an address space
    EngineState = 8,         ///< A channel's engine registers and the state behind them
    MacroContext = 9,        ///< The Switch 2 macro code shared by the channels
    Syncpoints = 10,         ///< Every syncpoint's guest and host value
    SyncpointObserve = 11,   ///< Guest syncpoint values the guest had seen by the next event
    SyncpointIncrement = 12, ///< An increment the GPU does not make (services, video)
    Submit = 13,             ///< GPU::PushGPUEntries
    FrameBoundary = 14,      ///< GPU::GuestFrameBoundary
    Composite = 15,          ///< GPU::RequestComposite
    Screenshot = 16,         ///< The image a composite showed, as the recording run saw it
    InitEnd = 17,            ///< The state above is complete; the stream follows
};

struct PhysPages {
    /// Raw physical page numbers (offset into the emulated DRAM >> PageBits).
    std::vector<u32> pages;
    /// PageSize bytes per page, in the order of `pages`.
    std::vector<u8> data;
};

struct SmmuRun {
    u64 device_page;
    u32 phys_page;
    u32 count; ///< Pages; device and physical pages both ascend by one
};

struct SmmuMap {
    std::vector<SmmuRun> runs;
};

struct SmmuUnmap {
    u64 address;
    u64 size;
};

struct ZeroReadRange {
    u64 address;
    u64 size;
};

struct AddressSpace {
    u64 id;
    u64 address_space_bits;
    u64 split_address;
    u64 big_page_bits;
    u64 page_bits;
    bool native_tiling;
    u64 syncpoint_shim;
};

enum class GmmuOpKind : u8 {
    Map = 0,
    MapSparse = 1,
    Unmap = 2,
};

struct GmmuOp {
    GmmuOpKind kind;
    bool big_pages;
    u8 pte_kind;
    u64 address_space;
    u64 gpu_addr;
    u64 device_addr;
    u64 size;
};

struct GmmuOps {
    /// 0: made at this point of the stream. Otherwise made by the GPU-ordered operation (GPU
    /// MapInOrder/RunInOrder) queued at this point, which a replay queues the same way.
    u64 ordered_id;
    bool before_later_reads;
    std::vector<GmmuOp> ops;
};

struct Channel {
    s32 bind_id;
    u64 address_space;
    u64 program_id;
};

struct EngineState {
    s32 bind_id;
    std::vector<u8> blob; ///< engine_state.h
};

struct MacroContext {
    std::vector<u8> blob; ///< engine_state.h
};

struct Syncpoints {
    std::vector<u32> guest;
    std::vector<u32> host;
};

struct SyncpointObserve {
    std::vector<std::pair<u32, u32>> values; ///< id, guest value
};

struct SyncpointIncrement {
    u32 id;
    bool guest;
    bool host;
};

/// Tegra::CommandList, field by field.
struct Submit {
    s32 channel;
    std::vector<u64> command_lists;         ///< CommandListHeader::raw
    std::vector<u32> prefetch_command_list; ///< CommandHeader::argument
    std::vector<u64> submit_hashes;
    std::vector<u32> submitted_words;
    std::vector<u32> submitted_offsets;
    std::vector<u32> deferred_entries;
    u64 snapshot_suspensions;
};

struct FrameBoundary {
    u64 frame; ///< Frames since the recording started
};

/// Tegra::FramebufferConfig, field by field.
struct Layer {
    u64 address;
    u32 offset;
    u32 width;
    u32 height;
    u32 stride;
    u32 pixel_format;
    u32 transform_flags;
    s32 crop_left;
    s32 crop_top;
    s32 crop_right;
    s32 crop_bottom;
    u32 blending;
    bool native_tiling;
    u32 layer_stack_mask;
    u32 color_space;
    u16 linear_unit_luminance;
};

struct Composite {
    u64 index; ///< Composites since the recording started
    std::vector<Layer> layers;
    std::vector<std::pair<s32, u32>> fences;
};

struct Screenshot {
    u64 composite_index;
    u32 width;
    u32 height;
    std::vector<u8> png;
};

struct InitEnd {};

using Event =
    std::variant<PhysPages, SmmuMap, SmmuUnmap, ZeroReadRange, AddressSpace, GmmuOps, Channel,
                 EngineState, MacroContext, Syncpoints, SyncpointObserve, SyncpointIncrement,
                 Submit, FrameBoundary, Composite, Screenshot, InitEnd>;

[[nodiscard]] EventType TypeOf(const Event& event);

/// Little-endian byte stream helpers.
class ByteWriter {
public:
    void U8(u8 value) {
        bytes.push_back(value);
    }
    void U16(u16 value) {
        Raw(&value, sizeof(value));
    }
    void U32(u32 value) {
        Raw(&value, sizeof(value));
    }
    void U64(u64 value) {
        Raw(&value, sizeof(value));
    }
    void S32(s32 value) {
        Raw(&value, sizeof(value));
    }
    void Raw(const void* data, size_t size) {
        const auto* const begin = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }
    template <typename T>
    void Vector(const std::vector<T>& values) {
        static_assert(std::is_trivially_copyable_v<T>);
        U64(values.size());
        Raw(values.data(), values.size() * sizeof(T));
    }
    void Bytes(std::span<const u8> values) {
        U64(values.size());
        Raw(values.data(), values.size());
    }

    std::vector<u8> bytes;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const u8> bytes_) : bytes{bytes_} {}

    [[nodiscard]] bool Ok() const {
        return ok;
    }
    [[nodiscard]] bool AtEnd() const {
        return offset == bytes.size();
    }
    u8 U8() {
        u8 value{};
        Raw(&value, sizeof(value));
        return value;
    }
    u16 U16() {
        u16 value{};
        Raw(&value, sizeof(value));
        return value;
    }
    u32 U32() {
        u32 value{};
        Raw(&value, sizeof(value));
        return value;
    }
    u64 U64() {
        u64 value{};
        Raw(&value, sizeof(value));
        return value;
    }
    s32 S32() {
        s32 value{};
        Raw(&value, sizeof(value));
        return value;
    }
    void Raw(void* data, size_t size) {
        if (!ok || size > bytes.size() - offset) {
            ok = false;
            std::memset(data, 0, size);
            return;
        }
        std::memcpy(data, bytes.data() + offset, size);
        offset += size;
    }
    template <typename T>
    std::vector<T> Vector() {
        static_assert(std::is_trivially_copyable_v<T>);
        const u64 count = U64();
        if (!ok || count > (bytes.size() - offset) / sizeof(T)) {
            ok = false;
            return {};
        }
        std::vector<T> values(count);
        Raw(values.data(), count * sizeof(T));
        return values;
    }

private:
    std::span<const u8> bytes;
    size_t offset{};
    bool ok{true};
};

/// The payload of one event (EncodeEvent) and back (DecodeEvent; nullopt: malformed).
[[nodiscard]] std::vector<u8> EncodeEvent(const Event& event);
[[nodiscard]] std::optional<Event> DecodeEvent(EventType type, std::span<const u8> payload);

struct Header {
    u32 version{FormatVersion};
    /// "key=value" lines: program_id, build, switch2, nx2_driver, ...
    std::string metadata;
};

[[nodiscard]] std::string MetadataValue(std::string_view metadata, std::string_view key);

/// Writes a recording: the header, then each event as a record (zstd above 4 KiB).
class FileWriter {
public:
    [[nodiscard]] bool Open(const std::filesystem::path& path, const Header& header);
    [[nodiscard]] bool Write(const Event& event);
    /// A record whose payload is already encoded (and maybe compressed: `raw_size` != size).
    [[nodiscard]] bool WriteEncoded(EventType type, std::span<const u8> payload, u64 raw_size,
                                    bool compressed);
    [[nodiscard]] bool Close();
    [[nodiscard]] u64 BytesWritten() const {
        return written;
    }

private:
    std::FILE* file{};
    u64 written{};
};

/// Compresses an encoded payload for WriteEncoded (level 1: the recording stalls the GPU).
[[nodiscard]] std::vector<u8> CompressPayload(std::span<const u8> payload);

struct RecordingFile {
    Header header;
    std::vector<Event> events;
};

/// Reads a whole recording. `error` says why on nullopt.
[[nodiscard]] std::optional<RecordingFile> ReadFile(const std::filesystem::path& path,
                                                    std::string& error);

} // namespace VideoCommon::GpuRecord
