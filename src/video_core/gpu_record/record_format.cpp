// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <type_traits>

#include "common/zstd_compression.h"
#include "video_core/gpu_record/record_format.h"

namespace VideoCommon::GpuRecord {

namespace {

constexpr u32 FlagZstd = 1;
/// Payloads below this are stored as they are.
constexpr size_t CompressThreshold = 4096;
/// Largest raw payload a record may declare (an initial snapshot is written in chunks).
constexpr u64 MaxRawPayload = u64{4} << 30;

void EncodeLayer(ByteWriter& w, const Layer& layer) {
    w.U64(layer.address);
    w.U32(layer.offset);
    w.U32(layer.width);
    w.U32(layer.height);
    w.U32(layer.stride);
    w.U32(layer.pixel_format);
    w.U32(layer.transform_flags);
    w.S32(layer.crop_left);
    w.S32(layer.crop_top);
    w.S32(layer.crop_right);
    w.S32(layer.crop_bottom);
    w.U32(layer.blending);
    w.U8(layer.native_tiling ? 1 : 0);
    w.U32(layer.layer_stack_mask);
    w.U32(layer.color_space);
    w.U16(layer.linear_unit_luminance);
}

Layer DecodeLayer(ByteReader& r) {
    Layer layer{};
    layer.address = r.U64();
    layer.offset = r.U32();
    layer.width = r.U32();
    layer.height = r.U32();
    layer.stride = r.U32();
    layer.pixel_format = r.U32();
    layer.transform_flags = r.U32();
    layer.crop_left = r.S32();
    layer.crop_top = r.S32();
    layer.crop_right = r.S32();
    layer.crop_bottom = r.S32();
    layer.blending = r.U32();
    layer.native_tiling = r.U8() != 0;
    layer.layer_stack_mask = r.U32();
    layer.color_space = r.U32();
    layer.linear_unit_luminance = r.U16();
    return layer;
}

void EncodeGmmuOp(ByteWriter& w, const GmmuOp& op) {
    w.U8(static_cast<u8>(op.kind));
    w.U8(op.big_pages ? 1 : 0);
    w.U8(op.pte_kind);
    w.U64(op.address_space);
    w.U64(op.gpu_addr);
    w.U64(op.device_addr);
    w.U64(op.size);
}

GmmuOp DecodeGmmuOp(ByteReader& r) {
    GmmuOp op{};
    op.kind = static_cast<GmmuOpKind>(r.U8());
    op.big_pages = r.U8() != 0;
    op.pte_kind = r.U8();
    op.address_space = r.U64();
    op.gpu_addr = r.U64();
    op.device_addr = r.U64();
    op.size = r.U64();
    return op;
}

template <typename T>
struct TypeOfEvent;
#define GPU_RECORD_TYPE(T)                                                                         \
    template <>                                                                                    \
    struct TypeOfEvent<T> {                                                                        \
        static constexpr EventType value = EventType::T;                                           \
    };
GPU_RECORD_TYPE(PhysPages)
GPU_RECORD_TYPE(SmmuMap)
GPU_RECORD_TYPE(SmmuUnmap)
GPU_RECORD_TYPE(ZeroReadRange)
GPU_RECORD_TYPE(AddressSpace)
GPU_RECORD_TYPE(GmmuOps)
GPU_RECORD_TYPE(Channel)
GPU_RECORD_TYPE(EngineState)
GPU_RECORD_TYPE(MacroContext)
GPU_RECORD_TYPE(Syncpoints)
GPU_RECORD_TYPE(SyncpointObserve)
GPU_RECORD_TYPE(SyncpointIncrement)
GPU_RECORD_TYPE(Submit)
GPU_RECORD_TYPE(FrameBoundary)
GPU_RECORD_TYPE(Composite)
GPU_RECORD_TYPE(Screenshot)
GPU_RECORD_TYPE(InitEnd)
#undef GPU_RECORD_TYPE

void Encode(ByteWriter& w, const PhysPages& e) {
    w.Vector(e.pages);
    w.Vector(e.data);
}
void Encode(ByteWriter& w, const SmmuMap& e) {
    w.U64(e.runs.size());
    for (const SmmuRun& run : e.runs) {
        w.U64(run.device_page);
        w.U32(run.phys_page);
        w.U32(run.count);
    }
}
void Encode(ByteWriter& w, const SmmuUnmap& e) {
    w.U64(e.address);
    w.U64(e.size);
}
void Encode(ByteWriter& w, const ZeroReadRange& e) {
    w.U64(e.address);
    w.U64(e.size);
}
void Encode(ByteWriter& w, const AddressSpace& e) {
    w.U64(e.id);
    w.U64(e.address_space_bits);
    w.U64(e.split_address);
    w.U64(e.big_page_bits);
    w.U64(e.page_bits);
    w.U8(e.native_tiling ? 1 : 0);
    w.U64(e.syncpoint_shim);
}
void Encode(ByteWriter& w, const GmmuOps& e) {
    w.U64(e.ordered_id);
    w.U8(e.before_later_reads ? 1 : 0);
    w.U64(e.ops.size());
    for (const GmmuOp& op : e.ops) {
        EncodeGmmuOp(w, op);
    }
}
void Encode(ByteWriter& w, const Channel& e) {
    w.S32(e.bind_id);
    w.U64(e.address_space);
    w.U64(e.program_id);
}
void Encode(ByteWriter& w, const EngineState& e) {
    w.S32(e.bind_id);
    w.Vector(e.blob);
}
void Encode(ByteWriter& w, const MacroContext& e) {
    w.Vector(e.blob);
}
void Encode(ByteWriter& w, const Syncpoints& e) {
    w.Vector(e.guest);
    w.Vector(e.host);
}
void Encode(ByteWriter& w, const SyncpointObserve& e) {
    w.U64(e.values.size());
    for (const auto& [id, value] : e.values) {
        w.U32(id);
        w.U32(value);
    }
}
void Encode(ByteWriter& w, const SyncpointIncrement& e) {
    w.U32(e.id);
    w.U8(e.guest ? 1 : 0);
    w.U8(e.host ? 1 : 0);
}
void Encode(ByteWriter& w, const Submit& e) {
    w.S32(e.channel);
    w.Vector(e.command_lists);
    w.Vector(e.prefetch_command_list);
    w.Vector(e.submit_hashes);
    w.Vector(e.submitted_words);
    w.Vector(e.submitted_offsets);
    w.Vector(e.deferred_entries);
    w.U64(e.snapshot_suspensions);
}
void Encode(ByteWriter& w, const FrameBoundary& e) {
    w.U64(e.frame);
}
void Encode(ByteWriter& w, const Composite& e) {
    w.U64(e.index);
    w.U64(e.layers.size());
    for (const Layer& layer : e.layers) {
        EncodeLayer(w, layer);
    }
    w.U64(e.fences.size());
    for (const auto& [id, value] : e.fences) {
        w.S32(id);
        w.U32(value);
    }
}
void Encode(ByteWriter& w, const Screenshot& e) {
    w.U64(e.composite_index);
    w.U32(e.width);
    w.U32(e.height);
    w.Vector(e.png);
}
void Encode(ByteWriter&, const InitEnd&) {}

template <typename T>
T Decode(ByteReader& r);

template <>
PhysPages Decode<PhysPages>(ByteReader& r) {
    PhysPages e;
    e.pages = r.Vector<u32>();
    e.data = r.Vector<u8>();
    return e;
}
template <>
SmmuMap Decode<SmmuMap>(ByteReader& r) {
    SmmuMap e;
    const u64 count = r.U64();
    for (u64 i = 0; i < count && r.Ok(); ++i) {
        SmmuRun run{};
        run.device_page = r.U64();
        run.phys_page = r.U32();
        run.count = r.U32();
        e.runs.push_back(run);
    }
    return e;
}
template <>
SmmuUnmap Decode<SmmuUnmap>(ByteReader& r) {
    SmmuUnmap e{};
    e.address = r.U64();
    e.size = r.U64();
    return e;
}
template <>
ZeroReadRange Decode<ZeroReadRange>(ByteReader& r) {
    ZeroReadRange e{};
    e.address = r.U64();
    e.size = r.U64();
    return e;
}
template <>
AddressSpace Decode<AddressSpace>(ByteReader& r) {
    AddressSpace e{};
    e.id = r.U64();
    e.address_space_bits = r.U64();
    e.split_address = r.U64();
    e.big_page_bits = r.U64();
    e.page_bits = r.U64();
    e.native_tiling = r.U8() != 0;
    e.syncpoint_shim = r.U64();
    return e;
}
template <>
GmmuOps Decode<GmmuOps>(ByteReader& r) {
    GmmuOps e{};
    e.ordered_id = r.U64();
    e.before_later_reads = r.U8() != 0;
    const u64 count = r.U64();
    for (u64 i = 0; i < count && r.Ok(); ++i) {
        e.ops.push_back(DecodeGmmuOp(r));
    }
    return e;
}
template <>
Channel Decode<Channel>(ByteReader& r) {
    Channel e{};
    e.bind_id = r.S32();
    e.address_space = r.U64();
    e.program_id = r.U64();
    return e;
}
template <>
EngineState Decode<EngineState>(ByteReader& r) {
    EngineState e{};
    e.bind_id = r.S32();
    e.blob = r.Vector<u8>();
    return e;
}
template <>
MacroContext Decode<MacroContext>(ByteReader& r) {
    MacroContext e{};
    e.blob = r.Vector<u8>();
    return e;
}
template <>
Syncpoints Decode<Syncpoints>(ByteReader& r) {
    Syncpoints e;
    e.guest = r.Vector<u32>();
    e.host = r.Vector<u32>();
    return e;
}
template <>
SyncpointObserve Decode<SyncpointObserve>(ByteReader& r) {
    SyncpointObserve e;
    const u64 count = r.U64();
    for (u64 i = 0; i < count && r.Ok(); ++i) {
        const u32 id = r.U32();
        const u32 value = r.U32();
        e.values.emplace_back(id, value);
    }
    return e;
}
template <>
SyncpointIncrement Decode<SyncpointIncrement>(ByteReader& r) {
    SyncpointIncrement e{};
    e.id = r.U32();
    e.guest = r.U8() != 0;
    e.host = r.U8() != 0;
    return e;
}
template <>
Submit Decode<Submit>(ByteReader& r) {
    Submit e{};
    e.channel = r.S32();
    e.command_lists = r.Vector<u64>();
    e.prefetch_command_list = r.Vector<u32>();
    e.submit_hashes = r.Vector<u64>();
    e.submitted_words = r.Vector<u32>();
    e.submitted_offsets = r.Vector<u32>();
    e.deferred_entries = r.Vector<u32>();
    e.snapshot_suspensions = r.U64();
    return e;
}
template <>
FrameBoundary Decode<FrameBoundary>(ByteReader& r) {
    FrameBoundary e{};
    e.frame = r.U64();
    return e;
}
template <>
Composite Decode<Composite>(ByteReader& r) {
    Composite e{};
    e.index = r.U64();
    const u64 layers = r.U64();
    for (u64 i = 0; i < layers && r.Ok(); ++i) {
        e.layers.push_back(DecodeLayer(r));
    }
    const u64 fences = r.U64();
    for (u64 i = 0; i < fences && r.Ok(); ++i) {
        const s32 id = r.S32();
        const u32 value = r.U32();
        e.fences.emplace_back(id, value);
    }
    return e;
}
template <>
Screenshot Decode<Screenshot>(ByteReader& r) {
    Screenshot e{};
    e.composite_index = r.U64();
    e.width = r.U32();
    e.height = r.U32();
    e.png = r.Vector<u8>();
    return e;
}
template <>
InitEnd Decode<InitEnd>(ByteReader&) {
    return {};
}

template <typename T>
std::optional<Event> DecodeAs(std::span<const u8> payload) {
    ByteReader reader{payload};
    Event event{Decode<T>(reader)};
    if (!reader.Ok() || !reader.AtEnd()) {
        return std::nullopt;
    }
    return event;
}

bool WriteAll(std::FILE* file, const void* data, size_t size) {
    return size == 0 || std::fwrite(data, 1, size, file) == size;
}

bool ReadAll(std::FILE* file, void* data, size_t size) {
    return size == 0 || std::fread(data, 1, size, file) == size;
}

} // Anonymous namespace

EventType TypeOf(const Event& event) {
    return std::visit([](const auto& e) { return TypeOfEvent<std::decay_t<decltype(e)>>::value; },
                      event);
}

std::vector<u8> EncodeEvent(const Event& event) {
    ByteWriter writer;
    std::visit([&writer](const auto& e) { Encode(writer, e); }, event);
    return std::move(writer.bytes);
}

std::optional<Event> DecodeEvent(EventType type, std::span<const u8> payload) {
    switch (type) {
    case EventType::PhysPages:
        return DecodeAs<PhysPages>(payload);
    case EventType::SmmuMap:
        return DecodeAs<SmmuMap>(payload);
    case EventType::SmmuUnmap:
        return DecodeAs<SmmuUnmap>(payload);
    case EventType::ZeroReadRange:
        return DecodeAs<ZeroReadRange>(payload);
    case EventType::AddressSpace:
        return DecodeAs<AddressSpace>(payload);
    case EventType::GmmuOps:
        return DecodeAs<GmmuOps>(payload);
    case EventType::Channel:
        return DecodeAs<Channel>(payload);
    case EventType::EngineState:
        return DecodeAs<EngineState>(payload);
    case EventType::MacroContext:
        return DecodeAs<MacroContext>(payload);
    case EventType::Syncpoints:
        return DecodeAs<Syncpoints>(payload);
    case EventType::SyncpointObserve:
        return DecodeAs<SyncpointObserve>(payload);
    case EventType::SyncpointIncrement:
        return DecodeAs<SyncpointIncrement>(payload);
    case EventType::Submit:
        return DecodeAs<Submit>(payload);
    case EventType::FrameBoundary:
        return DecodeAs<FrameBoundary>(payload);
    case EventType::Composite:
        return DecodeAs<Composite>(payload);
    case EventType::Screenshot:
        return DecodeAs<Screenshot>(payload);
    case EventType::InitEnd:
        return DecodeAs<InitEnd>(payload);
    }
    return std::nullopt;
}

std::string MetadataValue(std::string_view metadata, std::string_view key) {
    size_t start = 0;
    while (start < metadata.size()) {
        const size_t end = std::min(metadata.find('\n', start), metadata.size());
        const std::string_view line = metadata.substr(start, end - start);
        if (line.size() > key.size() && line.substr(0, key.size()) == key &&
            line[key.size()] == '=') {
            return std::string{line.substr(key.size() + 1)};
        }
        start = end + 1;
    }
    return {};
}

std::vector<u8> CompressPayload(std::span<const u8> payload) {
    return Common::Compression::CompressDataZSTD(payload.data(), payload.size(), 1);
}

bool FileWriter::Open(const std::filesystem::path& path, const Header& header) {
#ifdef _WIN32
    file = _wfopen(path.wstring().c_str(), L"wb");
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    if (file == nullptr) {
        return false;
    }
    written = 0;
    const u32 metadata_size = static_cast<u32>(header.metadata.size());
    bool ok = WriteAll(file, FileMagic.data(), FileMagic.size());
    ok = ok && WriteAll(file, &header.version, sizeof(header.version));
    ok = ok && WriteAll(file, &metadata_size, sizeof(metadata_size));
    ok = ok && WriteAll(file, header.metadata.data(), header.metadata.size());
    written += FileMagic.size() + sizeof(u32) * 2 + header.metadata.size();
    return ok;
}

bool FileWriter::Write(const Event& event) {
    const std::vector<u8> payload = EncodeEvent(event);
    if (payload.size() >= CompressThreshold) {
        const std::vector<u8> compressed = CompressPayload(payload);
        if (!compressed.empty() && compressed.size() < payload.size()) {
            return WriteEncoded(TypeOf(event), compressed, payload.size(), true);
        }
    }
    return WriteEncoded(TypeOf(event), payload, payload.size(), false);
}

bool FileWriter::WriteEncoded(EventType type, std::span<const u8> payload, u64 raw_size,
                              bool compressed) {
    if (file == nullptr) {
        return false;
    }
    const u32 type_value = static_cast<u32>(type);
    const u32 flags = compressed ? FlagZstd : 0;
    const u64 stored = payload.size();
    bool ok = WriteAll(file, &type_value, sizeof(type_value));
    ok = ok && WriteAll(file, &flags, sizeof(flags));
    ok = ok && WriteAll(file, &stored, sizeof(stored));
    ok = ok && WriteAll(file, &raw_size, sizeof(raw_size));
    ok = ok && WriteAll(file, payload.data(), payload.size());
    written += sizeof(u32) * 2 + sizeof(u64) * 2 + payload.size();
    return ok;
}

bool FileWriter::Close() {
    if (file == nullptr) {
        return false;
    }
    const bool ok = std::fclose(file) == 0;
    file = nullptr;
    return ok;
}

std::optional<RecordingFile> ReadFile(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
    std::FILE* const file = _wfopen(path.wstring().c_str(), L"rb");
#else
    std::FILE* const file = std::fopen(path.string().c_str(), "rb");
#endif
    if (file == nullptr) {
        error = "cannot open the file";
        return std::nullopt;
    }
    struct Closer {
        std::FILE* file;
        ~Closer() {
            std::fclose(file);
        }
    } const closer{file};

    std::array<char, 8> magic{};
    RecordingFile recording;
    u32 metadata_size = 0;
    if (!ReadAll(file, magic.data(), magic.size()) || magic != FileMagic) {
        error = "not a GPU recording";
        return std::nullopt;
    }
    if (!ReadAll(file, &recording.header.version, sizeof(u32)) ||
        recording.header.version != FormatVersion) {
        error = "unsupported recording version";
        return std::nullopt;
    }
    if (!ReadAll(file, &metadata_size, sizeof(metadata_size)) || metadata_size > (1U << 20)) {
        error = "damaged header";
        return std::nullopt;
    }
    recording.header.metadata.resize(metadata_size);
    if (!ReadAll(file, recording.header.metadata.data(), metadata_size)) {
        error = "damaged header";
        return std::nullopt;
    }
    std::vector<u8> stored;
    while (true) {
        u32 type = 0;
        if (std::fread(&type, 1, sizeof(type), file) != sizeof(type)) {
            if (std::feof(file)) {
                break;
            }
            error = "read error";
            return std::nullopt;
        }
        u32 flags = 0;
        u64 stored_size = 0;
        u64 raw_size = 0;
        if (!ReadAll(file, &flags, sizeof(flags)) || !ReadAll(file, &stored_size, sizeof(u64)) ||
            !ReadAll(file, &raw_size, sizeof(u64)) || stored_size > MaxRawPayload ||
            raw_size > MaxRawPayload) {
            error = "damaged record header";
            return std::nullopt;
        }
        stored.resize(stored_size);
        if (!ReadAll(file, stored.data(), stored.size())) {
            error = "truncated record";
            return std::nullopt;
        }
        std::optional<Event> event;
        if ((flags & FlagZstd) != 0) {
            const auto raw = Common::Compression::TryDecompressDataZSTD(stored, raw_size);
            if (!raw || raw->size() != raw_size) {
                error = "damaged compressed record";
                return std::nullopt;
            }
            event = DecodeEvent(static_cast<EventType>(type), *raw);
        } else {
            event = DecodeEvent(static_cast<EventType>(type), stored);
        }
        if (!event) {
            error = "malformed record of type " + std::to_string(type);
            return std::nullopt;
        }
        recording.events.push_back(std::move(*event));
    }
    return recording;
}

} // namespace VideoCommon::GpuRecord
