// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/nup_shader_cache.h"
#include "video_core/shader_cache_io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <zstd.h>
#include "native_shader_fingerprint.h"

#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/thread_worker.h"
#include "common/uuid.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/vfs/vfs.h"
#include "shader_recompiler/frontend/ampere/archive_vertex_layout.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/shader_archive.h"

namespace VideoCommon {
namespace {
using Json = nlohmann::json;
using Instruction = Shader::Ampere::Instruction;
constexpr size_t MaxBytes = 512 * 1024 * 1024;
constexpr size_t HeaderSize = 56;
constexpr size_t MaxIndexBytes = 64 * 1024 * 1024;
constexpr u64 MaxDriverBytes = 8ULL * 1024 * 1024 * 1024;
constexpr std::array<u8, 8> CacheMagic{'L', 'C', 'H', 'N', 'S', 'C', '0', '3'};

std::span<const u8> Slice(std::span<const u8> data, size_t offset, size_t size) {
    if (offset > data.size() || size > data.size() - offset) {
        throw std::runtime_error("NUP shader data is out of bounds");
    }
    return data.subspan(offset, size);
}

template <typename T>
T Read(std::span<const u8> data, size_t offset) {
    T result;
    std::memcpy(&result, Slice(data, offset, sizeof(T)).data(), sizeof(T));
    return result;
}

std::array<u8, SHA256_DIGEST_LENGTH> Digest(std::span<const u8> data) {
    std::array<u8, SHA256_DIGEST_LENGTH> result;
    SHA256(data.data(), data.size(), result.data());
    return result;
}

template <typename T>
Json Bytes(const std::vector<T>& values) {
    const auto* first = reinterpret_cast<const u8*>(values.data());
    return Json::binary(std::vector<u8>(first, first + values.size() * sizeof(T)));
}

template <typename T>
std::vector<T> Values(const Json& value) {
    const auto& bytes = value.get_binary();
    if (bytes.size() % sizeof(T) || bytes.size() > MaxBytes) {
        throw std::runtime_error("Invalid NUP shader cache vector");
    }
    std::vector<T> result(bytes.size() / sizeof(T));
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

struct Member {
    std::string path;
    FileSys::VirtualFile file;
};

void Collect(const FileSys::VirtualDir& directory, const std::string& path,
             std::vector<Member>& files, size_t depth = 0, bool include_models = false) {
    if (!directory) {
        return;
    }
    if (depth > 128 || files.size() > 1000000) {
        throw std::runtime_error("NUP shader directory limit exceeded");
    }
    for (const auto& file : directory->GetFiles()) {
        auto name = path + "/" + file->GetName();
        if (name.find("Shader") != std::string::npos || name.ends_with(".bfsha") ||
            name.ends_with(".bfsha.zs") || name.ends_with(".bnsh") || name.ends_with(".bnsh.zs") ||
            name.ends_with(".vsetb.byml.zs") || name.ends_with("ZsDic.pack.zs") ||
            (include_models && (name.ends_with(".bfres") || name.ends_with(".bfres.zs")))) {
            files.push_back({std::move(name), file});
        }
    }
    for (const auto& child : directory->GetSubdirectories()) {
        Collect(child, path + "/" + child->GetName(), files, depth + 1, include_models);
    }
}

std::vector<u8> Decompress(std::span<const u8> data,
                           const std::map<u32, std::vector<u8>>& dictionaries) {
    if (data.size() < 4 || Read<u32>(data, 0) != ZSTD_MAGICNUMBER) {
        return {data.begin(), data.end()};
    }
    const auto size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (size > MaxBytes) {
        throw std::runtime_error("Unknown or excessive NUP shader decompressed size");
    }
    std::vector<u8> result(static_cast<size_t>(size));
    const u32 id = ZSTD_getDictID_fromFrame(data.data(), data.size());
    const auto dictionary = dictionaries.find(id);
    if (id && dictionary == dictionaries.end()) {
        throw std::runtime_error("Missing NUP shader Zstandard dictionary");
    }
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{ZSTD_createDCtx(), ZSTD_freeDCtx};
    if (!context) {
        throw std::bad_alloc{};
    }
    const auto* dict = id ? dictionary->second.data() : nullptr;
    const size_t dict_size = id ? dictionary->second.size() : 0;
    const size_t written = ZSTD_decompress_usingDict(context.get(), result.data(), result.size(),
                                                     data.data(), data.size(), dict, dict_size);
    if (ZSTD_isError(written) || written != result.size()) {
        throw std::runtime_error("NUP shader decompression failed");
    }
    return result;
}

void ReadDictionaries(std::span<const u8> data, std::map<u32, std::vector<u8>>& dictionaries) {
    // ZsDic packs are little-endian SARC archives. Reject other layouts explicitly.
    if (Read<u32>(data, 0) != 0x43524153 || Read<u16>(data, 6) != 0xfeff ||
        Read<u32>(data, 8) != data.size()) {
        throw std::runtime_error("Unsupported NUP shader dictionary pack");
    }
    const size_t sfat = Read<u16>(data, 4);
    if (Read<u32>(data, sfat) != 0x54414653) {
        throw std::runtime_error("Invalid NUP shader dictionary table");
    }
    const size_t nodes = sfat + Read<u16>(data, sfat + 4);
    const size_t count = Read<u16>(data, sfat + 6);
    const size_t payload = Read<u32>(data, 12);
    Slice(data, nodes, count * 16);
    for (size_t i = 0; i < count; ++i) {
        const size_t begin = Read<u32>(data, nodes + i * 16 + 8);
        const size_t end = Read<u32>(data, nodes + i * 16 + 12);
        if (end < begin || begin > data.size() - std::min(payload, data.size())) {
            throw std::runtime_error("Invalid NUP shader dictionary extent");
        }
        const auto bytes = Slice(data, payload + begin, end - begin);
        const u32 id = ZSTD_getDictID_fromDict(bytes.data(), bytes.size());
        if (id) {
            std::vector<u8> dictionary(bytes.begin(), bytes.end());
            const auto [it, inserted] = dictionaries.emplace(id, dictionary);
            if (!inserted && it->second != dictionary) {
                throw std::runtime_error("Conflicting NUP shader dictionary IDs");
            }
        }
    }
}

struct SourceArchives {
    std::vector<Member> files;
    std::map<u32, std::vector<u8>> dictionaries;
};

SourceArchives ReadSourceArchives(FileSys::NupContainer& nup, std::stop_token stop,
                                  bool include_models = false) {
    SourceArchives result;
    FileSys::VirtualDir romfs;
    if (const auto base = nup.FindBasePackage())
        romfs = nup.GetRomFSDir(*base);
    if (const auto update = nup.FindBestUpdatePackage()) {
        romfs = FileSys::NupContainer::MergeShadowDirs(romfs, nup.GetRomFSDir(*update));
    }
    Collect(romfs, "", result.files, 0, include_models);
    for (size_t i = 0; i < nup.GetPackages().size(); ++i) {
        if (nup.GetPackages()[i].kind == FileSys::NupContainer::PackageKind::Addon) {
            Collect(nup.GetRomFSDir(i), "addon/" + std::to_string(i), result.files, 0,
                    include_models);
        }
    }
    for (const auto& member : result.files) {
        if (stop.stop_requested())
            break;
        if (member.path.ends_with("ZsDic.pack.zs")) {
            if (member.file->GetSize() > MaxBytes) {
                throw std::runtime_error("NUP dictionary pack exceeds memory limit");
            }
            ReadDictionaries(Decompress(member.file->ReadAllBytes(), result.dictionaries),
                             result.dictionaries);
        }
    }
    return result;
}
} // namespace

struct NupShaderCache::Impl {
    struct Entry {
        Shader::Stage stage;
        std::array<u8, SHA256_DIGEST_LENGTH> native_digest;
        u64 offset;
        u64 size;
        std::array<u8, SHA256_DIGEST_LENGTH> checksum;
    };
    std::filesystem::path path;
    FileSys::VirtualFile source;
    std::array<u8, 32> identity{};
    std::array<u8, HeaderSize> header{};
    Json index;
    u64 native_end{HeaderSize};
    u64 index_offset{};
    mutable std::mutex driver_mutex;
    std::optional<std::array<u8, 32>> verified_driver_checksum;
    std::unordered_multimap<u64, Entry> entries;
    Json pending = Json::array();
    using NamedVertexTypes = std::vector<std::pair<std::string, u8>>;
    std::map<std::pair<std::string, std::string>, std::set<NamedVertexTypes>> vertex_layouts;
    bool vertex_layouts_ready{};

    const Entry* Find(std::span<const Instruction> native, Shader::Stage stage) const {
        const auto [first, last] = entries.equal_range(Shader::Ampere::NativeProgramKey(native));
        if (first == last) {
            return nullptr;
        }
        const auto digest =
            Digest({reinterpret_cast<const u8*>(native.data()), native.size_bytes()});
        for (auto it = first; it != last; ++it) {
            if (it->second.stage == stage && it->second.native_digest == digest) {
                return &it->second;
            }
        }
        return nullptr;
    }
};

NupShaderCache::NupShaderCache() : impl{std::make_unique<Impl>()} {}
NupShaderCache::~NupShaderCache() = default;

std::optional<std::array<u8, 32>> NupShaderCache::Identity() const {
    return impl->path.empty() ? std::nullopt : std::optional{impl->identity};
}

bool NupShaderCache::HasProgram(std::span<const Instruction> native, Shader::Stage stage) const {
    return impl->Find(native, stage) != nullptr;
}

std::optional<std::vector<u8>> NupShaderCache::ReadDriverCache(
    const std::array<u8, 32>& profile) const try {
    std::scoped_lock lock{impl->driver_mutex};
    if (impl->path.empty() || !impl->index.contains("driver"))
        return std::nullopt;
    const auto& record = impl->index.at("driver");
    const auto saved_profile = Values<u8>(record.at("profile"));
    if (!std::ranges::equal(profile, saved_profile))
        return std::nullopt;
    const u64 offset = record.at("offset");
    const u64 size = record.at("size");
    const u64 raw_size = record.at("raw_size");
    if (offset < impl->native_end || offset > impl->index_offset || !size ||
        size > impl->index_offset - offset || size > ZSTD_compressBound(MaxDriverBytes) ||
        !raw_size || raw_size > MaxDriverBytes) {
        throw std::runtime_error("Invalid embedded driver cache extent");
    }
    std::ifstream file(impl->path, std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    std::vector<u8> compressed(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(compressed.data()),
                   static_cast<std::streamsize>(compressed.size()))) {
        throw std::runtime_error("Cannot read embedded driver cache");
    }
    if (record.contains("compressed_checksum") &&
        !std::ranges::equal(Digest(compressed), Values<u8>(record.at("compressed_checksum")))) {
        throw std::runtime_error("Embedded driver record checksum mismatch");
    }
    if (ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != raw_size) {
        throw std::runtime_error("Embedded driver cache size mismatch");
    }
    std::vector<u8> data(static_cast<size_t>(raw_size));
    const size_t written =
        ZSTD_decompress(data.data(), data.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(written) || written != data.size()) {
        throw std::runtime_error("Embedded driver cache decompression failed");
    }
    const auto checksum = Digest(data);
    if (!std::ranges::equal(checksum, Values<u8>(record.at("checksum")))) {
        throw std::runtime_error("Embedded driver cache checksum mismatch");
    }
    impl->verified_driver_checksum = checksum;
    return data;
} catch (const std::exception& error) {
    LOG_WARNING(Shader, "Embedded NUP driver cache ignored: {}", error.what());
    return std::nullopt;
}

bool NupShaderCache::WriteDriverCache(const std::array<u8, 32>& profile, std::span<const u8> data,
                                      std::stop_token stop) try {
    ShaderCacheCriticalScope save_scope;
    std::scoped_lock lock{impl->driver_mutex};
    if (impl->path.empty() || data.empty() || data.size() > MaxDriverBytes || stop.stop_requested())
        return false;
    std::ifstream source(impl->path, std::ios::binary);
    std::array<u8, HeaderSize> current_header;
    if (!source.read(reinterpret_cast<char*>(current_header.data()), current_header.size()) ||
        current_header != impl->header)
        return false;
    const auto modified = std::filesystem::last_write_time(impl->path);
    const auto checksum = Digest(data);
    std::vector<char> block(1024 * 1024);
    if (impl->index.contains("driver")) {
        const auto& old = impl->index.at("driver");
        if (impl->verified_driver_checksum == checksum && old.contains("compressed_checksum") &&
            std::ranges::equal(profile, Values<u8>(old.at("profile"))) &&
            std::ranges::equal(checksum, Values<u8>(old.at("checksum")))) {
            const u64 offset = old.at("offset"), size = old.at("size");
            if (offset >= impl->native_end && offset <= impl->index_offset &&
                size <= impl->index_offset - offset) {
                source.seekg(static_cast<std::streamoff>(offset));
                SHA256_CTX context;
                SHA256_Init(&context);
                u64 remaining = size;
                while (remaining && source) {
                    if (stop.stop_requested())
                        return false;
                    const auto amount =
                        static_cast<std::streamsize>(std::min<u64>(remaining, block.size()));
                    if (!source.read(block.data(), amount))
                        break;
                    SHA256_Update(&context, block.data(), static_cast<size_t>(amount));
                    remaining -= amount;
                    ShaderCacheWriteProgress();
                }
                std::array<u8, 32> stored_checksum;
                SHA256_Final(stored_checksum.data(), &context);
                if (!remaining && std::ranges::equal(stored_checksum,
                                                     Values<u8>(old.at("compressed_checksum")))) {
                    std::ifstream current(impl->path, std::ios::binary);
                    std::array<u8, HeaderSize> latest_header;
                    return !stop.stop_requested() &&
                           current.read(reinterpret_cast<char*>(latest_header.data()),
                                        latest_header.size()) &&
                           latest_header == impl->header &&
                           std::filesystem::last_write_time(impl->path) == modified;
                }
            }
        }
    }
    std::vector<u8> compressed(ZSTD_compressBound(data.size()));
    const size_t size =
        ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), 3);
    if (ZSTD_isError(size))
        return false;
    ShaderCacheWriteProgress();
    compressed.resize(size);
    if (stop.stop_requested())
        return false;
    Json index = impl->index;
    const auto compressed_checksum = Digest(compressed);
    index["driver"] = {
        {"profile", Json::binary(std::vector<u8>(profile.begin(), profile.end()))},
        {"offset", impl->native_end},
        {"size", compressed.size()},
        {"raw_size", data.size()},
        {"checksum", Json::binary(std::vector<u8>(checksum.begin(), checksum.end()))},
        {"compressed_checksum",
         Json::binary(std::vector<u8>(compressed_checksum.begin(), compressed_checksum.end()))}};
    const auto encoded = Json::to_cbor(index);
    if (encoded.size() > MaxIndexBytes)
        return false;
    auto temporary = impl->path;
    temporary += "." + Common::UUID::MakeRandom().RawString() + ".pending";
    SCOPE_EXIT {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    };
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    source.clear();
    source.seekg(0);
    u64 remaining = impl->native_end;
    while (remaining) {
        if (stop.stop_requested())
            return false;
        const auto amount = static_cast<std::streamsize>(std::min<u64>(remaining, block.size()));
        if (!source.read(block.data(), amount))
            return false;
        output.write(block.data(), amount);
        remaining -= amount;
        ShaderCacheWriteProgress();
    }
    output.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    const u64 index_offset = impl->native_end + compressed.size();
    const u64 index_size = encoded.size();
    output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    auto header = impl->header;
    std::memcpy(header.data() + 8, &index_offset, sizeof(index_offset));
    std::memcpy(header.data() + 16, &index_size, sizeof(index_size));
    const auto index_checksum = Digest(encoded);
    std::copy(index_checksum.begin(), index_checksum.end(), header.begin() + 24);
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.close();
    source.close();
    if (stop.stop_requested() || std::filesystem::last_write_time(impl->path) != modified)
        return false;
    std::filesystem::rename(temporary, impl->path);
    impl->header = header;
    impl->index_offset = index_offset;
    impl->index = std::move(index);
    impl->verified_driver_checksum = checksum;
    LOG_INFO(Shader, "Stored {} bytes of compiled driver cache in {}", data.size(),
             impl->path.string());
    return true;
} catch (const std::exception& error) {
    LOG_WARNING(Shader, "Cannot embed NUP driver cache: {}", error.what());
    return false;
}

bool NupShaderCache::PrepareVertexLayouts(std::stop_token stop,
                                          const std::function<void(size_t, size_t)>& progress) try {
    if (impl->vertex_layouts_ready)
        return true;
    if (!impl->source || stop.stop_requested())
        return false;
    impl->vertex_layouts.clear();
    FileSys::NupContainer nup{impl->source};
    if (nup.GetStatus() != Loader::ResultStatus::Success)
        return false;
    const auto archives = ReadSourceArchives(nup, stop, true);
    const auto is_model = [](const Member& member) {
        return member.path.ends_with(".bfres") || member.path.ends_with(".bfres.zs");
    };
    const size_t total = std::ranges::count_if(archives.files, is_model);
    size_t done = 0, unsupported = 0, layouts = 0;
    std::map<std::string, std::pair<size_t, std::string>> rejected;
    progress(0, total);
    for (const auto& member : archives.files) {
        if (stop.stop_requested())
            return false;
        if (!is_model(member))
            continue;
        if (member.file->GetSize() > MaxBytes) {
            ++unsupported;
            progress(++done, total);
            continue;
        }
        const auto stored = member.file->ReadAllBytes();
        if (stored.size() != member.file->GetSize())
            return false;
        const auto bytes = Decompress(stored, archives.dictionaries);
        try {
            for (const auto& layout : Shader::Ampere::ReadBfresVertexLayouts(bytes)) {
                Impl::NamedVertexTypes types;
                for (const auto& attribute : layout.attributes) {
                    types.emplace_back(attribute.shader_name, static_cast<u8>(attribute.type) + 1);
                }
                std::ranges::sort(types);
                if (!types.empty()) {
                    layouts += impl->vertex_layouts[{layout.shader_archive, layout.shading_model}]
                                   .insert(std::move(types))
                                   .second;
                }
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            ++unsupported;
            auto& [count, example] = rejected[error.what()];
            if (count++ == 0) example = member.path;
        }
        progress(++done, total);
    }
    if (stop.stop_requested())
        return false;
    impl->vertex_layouts_ready = true;
    LOG_INFO(Shader,
             "NUP vertex layouts: {} distinct material layouts from {} models; {} unsupported",
             layouts, done, unsupported);
    for (const auto& [reason, group] : rejected) {
        LOG_INFO(Shader, "NUP vertex layouts deferred: {} files: {}; example {}",
                 group.first, reason, group.second);
    }
    return true;
} catch (const std::exception& error) {
    LOG_WARNING(Shader, "NUP vertex layout scan failed: {}", error.what());
    return false;
}

bool NupShaderCache::ForEachGraphicsGroup(
    std::stop_token stop,
    const std::function<void(std::span<const Shader::Ampere::ArchiveProgram>)>& callback) const {
    return ForEachGraphicsGroupWithVertexLayouts(stop, [&](auto group, auto) { callback(group); });
}

bool NupShaderCache::ForEachGraphicsGroupWithVertexLayouts(
    std::stop_token stop,
    const std::function<void(std::span<const Shader::Ampere::ArchiveProgram>,
                             std::span<const VertexInputTypes>)>& callback) const {
    if (!impl->source || stop.stop_requested())
        return false;
    FileSys::NupContainer nup{impl->source};
    if (nup.GetStatus() != Loader::ResultStatus::Success)
        return false;
    const auto archives = ReadSourceArchives(nup, stop);
    for (const auto& member : archives.files) {
        if (stop.stop_requested())
            return false;
        if (member.path.ends_with("ZsDic.pack.zs"))
            continue;
        bool decoding = true;
        try {
            if (member.file->GetSize() > MaxBytes)
                throw std::runtime_error("Shader archive exceeds memory limit");
            const auto stored = member.file->ReadAllBytes();
            if (stored.size() != member.file->GetSize())
                return false;
            const auto bytes = Decompress(stored, archives.dictionaries);
            decoding = false;
            std::map<u64, std::vector<VertexInputTypes>> inputs;
            if (impl->vertex_layouts_ready && bytes.size() >= 4 &&
                std::memcmp(bytes.data(), "FSHA", 4) == 0) {
                try {
                    for (const auto& shader : Shader::Ampere::ReadBfshaVertexAttributeMaps(bytes)) {
                        const auto found = impl->vertex_layouts.find(
                            {shader.shader_archive, shader.shading_model});
                        if (found == impl->vertex_layouts.end())
                            continue;
                        std::set<VertexInputTypes> distinct;
                        for (const auto& layout : found->second) {
                            VertexInputTypes types{};
                            bool valid = true;
                            for (const auto& [name, type] : layout) {
                                const auto attribute = std::ranges::find(
                                    shader.attributes, name,
                                    &Shader::Ampere::ArchiveAttributeLocation::name);
                                if (attribute == shader.attributes.end()) {
                                    valid = false;
                                    break;
                                }
                                if (attribute->location >= 0)
                                    types[attribute->location] = type;
                            }
                            if (valid)
                                distinct.insert(types);
                        }
                        inputs[shader.shader_binary_offset] = {distinct.begin(), distinct.end()};
                    }
                } catch (const std::bad_alloc&) {
                    throw;
                } catch (const std::exception& error) {
                    LOG_DEBUG(Shader, "NUP vertex mapping {}: {}", member.path, error.what());
                }
            }
            std::map<std::pair<size_t, u32>, std::vector<Shader::Ampere::ArchiveProgram>> groups;
            Shader::Ampere::ReadShaderArchive(bytes, [&](const auto& program) {
                if (stop.stop_requested())
                    return false;
                if (program.variation) {
                    const size_t offset = program.container.data() - bytes.data();
                    groups[{offset, *program.variation}].push_back(program);
                }
                return true;
            });
            for (const auto& [key, group] : groups) {
                if (stop.stop_requested())
                    return false;
                if (std::ranges::any_of(group, [](const auto& program) {
                        return program.stage != Shader::Stage::Compute;
                    })) {
                    const auto found = inputs.find(key.first);
                    const std::span<const VertexInputTypes> types =
                        found == inputs.end() ? std::span<const VertexInputTypes>{} : found->second;
                    callback(group, types);
                }
            }
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::exception& error) {
            LOG_WARNING(Shader, "NUP graphics enumeration {}: {}", member.path, error.what());
            if (decoding)
                return false;
        }
    }
    return !stop.stop_requested();
}

void NupShaderCache::ForEachComputeVariant(
    std::stop_token stop,
    const std::function<void(std::span<const u8>, std::span<const u8>,
                             const Shader::Ampere::LoweredProgram*)>& callback) const {
    if (!impl->source || stop.stop_requested())
        return;
    FileSys::NupContainer nup{impl->source};
    if (nup.GetStatus() != Loader::ResultStatus::Success)
        return;
    const auto archives = ReadSourceArchives(nup, stop);
    for (const auto& member : archives.files) {
        if (stop.stop_requested())
            return;
        if (member.path.ends_with("ZsDic.pack.zs"))
            continue;
        try {
            if (member.file->GetSize() > MaxBytes)
                throw std::runtime_error("Shader archive exceeds memory limit");
            const auto bytes = Decompress(member.file->ReadAllBytes(), archives.dictionaries);
            Shader::Ampere::ReadShaderArchive(bytes, [&](const auto& program) {
                if (stop.stop_requested())
                    return false;
                if (program.stage != Shader::Stage::Compute)
                    return true;
                const size_t count = std::min(
                    program.code.size() / sizeof(Instruction),
                    static_cast<size_t>(Shader::Ampere::ProgramCaptureState::MaximumInstructions));
                std::vector<Instruction> native(count);
                std::memcpy(native.data(), program.code.data(), count * sizeof(Instruction));
                Shader::Ampere::ProgramCaptureState capture;
                for (size_t i = 0; i < count; ++i) {
                    const auto state = capture.Observe(native[i], static_cast<u32>(i));
                    if (state == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget)
                        break;
                    if (state == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                        native.resize(i + 1);
                        const auto lowered = Find(native, Shader::Stage::Compute, false);
                        callback(program.code, program.control, lowered.get());
                        return true;
                    }
                }
                callback(program.code, program.control, nullptr);
                return true;
            });
        } catch (const std::exception& error) {
            LOG_WARNING(Shader, "NUP compute enumeration {}: {}", member.path, error.what());
        }
    }
}

std::unique_ptr<Shader::Ampere::LoweredProgram> NupShaderCache::Find(
    std::span<const Instruction> native, Shader::Stage stage, bool geometry) const try {
    if (geometry != (stage == Shader::Stage::Geometry)) {
        return nullptr;
    }
    const auto* entry = impl->Find(native, stage);
    if (!entry) {
        return nullptr;
    }
    std::ifstream stream(impl->path, std::ios::binary);
    stream.seekg(static_cast<std::streamoff>(entry->offset));
    std::vector<u8> compressed(static_cast<size_t>(entry->size));
    stream.read(reinterpret_cast<char*>(compressed.data()),
                static_cast<std::streamsize>(compressed.size()));
    if (!stream || Digest(compressed) != entry->checksum) {
        throw std::runtime_error("NUP shader record checksum mismatch");
    }
    const auto bytes = Decompress(compressed, {});
    const auto record = Json::from_cbor(bytes);
    const auto source = Values<Instruction>(record.at("native"));
    if (source.size() != native.size() ||
        std::memcmp(source.data(), native.data(), native.size_bytes()) != 0) {
        return nullptr;
    }
    auto lowered = std::make_unique<Shader::Ampere::LoweredProgram>();
    lowered->instructions = Values<u64>(record.at("instructions"));
    lowered->source_instruction_offsets = Values<u32>(record.at("offsets"));
    lowered->source_size = record.at("source_size");
    lowered->required_texture_buffer = record.at("texture_buffer");
    if (lowered->instructions.empty() || lowered->source_size > native.size_bytes() ||
        lowered->source_instruction_offsets.size() > native.size() ||
        std::ranges::any_of(lowered->source_instruction_offsets,
                            [&](u32 offset) { return offset > lowered->instructions.size(); })) {
        return nullptr;
    }
    return lowered;
} catch (const std::exception& error) {
    LOG_WARNING(Shader, "Ignoring invalid NUP shader cache record: {}", error.what());
    return nullptr;
}

bool NupShaderCache::Prepare(const FileSys::VirtualFile& file, const std::filesystem::path& path,
                             std::stop_token stop,
                             const std::function<void(size_t, size_t)>& progress) try {
    impl = std::make_unique<Impl>();
    if (!FileSys::NupContainer::IsNupFile(file)) {
        return false;
    }
    impl->source = file;
    FileSys::NupContainer nup{file};
    if (nup.GetStatus() != Loader::ResultStatus::Success) {
        throw std::runtime_error("NUP shader preloading cannot read container");
    }
    auto cache_path = path;
    cache_path.replace_extension(".shaders.bin");
    const auto source_identity = [&] {
        return Json{{"header", Bytes(file->ReadBytes(128))},
                    {"size", file->GetSize()},
                    {"modified", std::filesystem::last_write_time(path).time_since_epoch().count()},
                    {"compiler", LICHII_SHADER_COMPILER_FINGERPRINT},
                    {"format", 3}};
    };
    const Json identity = source_identity();
    const auto identity_digest = Digest(Json::to_cbor(identity));
    impl->identity = identity_digest;
    if (std::filesystem::is_regular_file(cache_path)) {
        try {
            std::ifstream stream(cache_path, std::ios::binary);
            std::array<u8, HeaderSize> header{};
            stream.read(reinterpret_cast<char*>(header.data()), header.size());
            const u64 file_size = std::filesystem::file_size(cache_path);
            if (!stream || !std::equal(CacheMagic.begin(), CacheMagic.end(), header.begin())) {
                throw std::runtime_error("Invalid NUP shader cache header");
            }
            const u64 index_offset = Read<u64>(header, 8);
            const u64 index_size = Read<u64>(header, 16);
            if (index_offset < HeaderSize || index_offset > file_size ||
                index_size > MaxIndexBytes || index_size != file_size - index_offset) {
                throw std::runtime_error("Invalid NUP shader index extent");
            }
            std::vector<u8> data(static_cast<size_t>(index_size));
            stream.seekg(static_cast<std::streamoff>(index_offset));
            stream.read(reinterpret_cast<char*>(data.data()),
                        static_cast<std::streamsize>(data.size()));
            const auto digest = Digest(data);
            if (!stream || !std::equal(digest.begin(), digest.end(), header.begin() + 24)) {
                throw std::runtime_error("NUP shader index checksum mismatch");
            }
            const auto cache = Json::from_cbor(data);
            if (cache.at("identity") != identity) {
                throw std::runtime_error("NUP shader cache version or source changed");
            }
            for (const auto& row : cache.at("programs")) {
                if (stop.stop_requested()) {
                    impl = std::make_unique<Impl>();
                    return false;
                }
                Impl::Entry entry{static_cast<Shader::Stage>(row.at("stage").get<u32>()),
                                  {},
                                  row.at("offset").get<u64>(),
                                  row.at("size").get<u64>(),
                                  {}};
                const auto digest = Values<u8>(row.at("native_digest"));
                const auto checksum = Values<u8>(row.at("checksum"));
                if (static_cast<u32>(entry.stage) > static_cast<u32>(Shader::Stage::Mesh) ||
                    digest.size() != entry.native_digest.size() ||
                    checksum.size() != entry.checksum.size() || entry.size == 0 ||
                    entry.size > MaxBytes || entry.offset < HeaderSize ||
                    entry.offset > index_offset || entry.size > index_offset - entry.offset) {
                    throw std::runtime_error("Invalid NUP precompiled shader record");
                }
                std::copy(digest.begin(), digest.end(), entry.native_digest.begin());
                std::copy(checksum.begin(), checksum.end(), entry.checksum.begin());
                impl->native_end = std::max(impl->native_end, entry.offset + entry.size);
                impl->entries.emplace(row.at("key").get<u64>(), std::move(entry));
            }
            impl->pending = cache.at("pending");
            impl->header = header;
            impl->index_offset = index_offset;
            impl->index = cache;
            impl->path = cache_path;
            progress(1, 1);
            LOG_INFO(Shader, "NUP shader preload: reused {} lowered programs, {} pending",
                     impl->entries.size(), impl->pending.size());
            return true;
        } catch (const std::exception& error) {
            LOG_WARNING(Shader, "Rebuilding NUP shader cache: {}", error.what());
            impl = std::make_unique<Impl>();
            impl->source = file;
            impl->identity = identity_digest;
        }
    }
    const auto archives = ReadSourceArchives(nup, stop);
    const auto& files = archives.files;
    const auto& dictionaries = archives.dictionaries;
    auto temporary = cache_path;
    temporary += "." + Common::UUID::MakeRandom().RawString() + ".pending";
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    };
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    std::array<u8, HeaderSize> header{};
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    if (!output) {
        throw std::runtime_error("Cannot create NUP shader cache");
    }
    std::mutex writer_mutex;
    std::atomic<bool> write_failed{};
    struct Job {
        Shader::Stage stage;
        std::vector<Instruction> native;
        std::string archive;
        std::array<u8, SHA256_DIGEST_LENGTH> digest;
    };
    std::set<std::pair<u32, std::array<u8, SHA256_DIGEST_LENGTH>>> seen;
    const auto compile = [&](const std::shared_ptr<Job>& job) {
        if (stop.stop_requested() || write_failed.load()) {
            return;
        }
        try {
            const auto tessellation = job->stage == Shader::Stage::TessellationControl
                                          ? Shader::Ampere::TessellationStage::Control
                                      : job->stage == Shader::Stage::TessellationEval
                                          ? Shader::Ampere::TessellationStage::Evaluation
                                          : Shader::Ampere::TessellationStage::None;
            auto lowered = Shader::Ampere::LowerToMaxwell(
                job->native, {.geometry = job->stage == Shader::Stage::Geometry,
                              .tensor = true,
                              .tessellation_stage = tessellation});
            if (!lowered) {
                throw std::runtime_error(std::string{lowered.error});
            }
            const auto data =
                Json::to_cbor(Json{{"native", Bytes(job->native)},
                                   {"instructions", Bytes(lowered.instructions)},
                                   {"offsets", Bytes(lowered.source_instruction_offsets)},
                                   {"source_size", lowered.source_size},
                                   {"texture_buffer", lowered.required_texture_buffer}});
            std::vector<u8> compressed(ZSTD_compressBound(data.size()));
            const size_t size =
                ZSTD_compress(compressed.data(), compressed.size(), data.data(), data.size(), 3);
            if (ZSTD_isError(size) || size > MaxBytes) {
                throw std::runtime_error("NUP shader record exceeds size limit");
            }
            compressed.resize(size);
            const u64 key = Shader::Ampere::NativeProgramKey(job->native);
            const auto checksum = Digest(compressed);
            std::scoped_lock lock{writer_mutex};
            const auto offset = output.tellp();
            output.write(reinterpret_cast<const char*>(compressed.data()),
                         static_cast<std::streamsize>(compressed.size()));
            if (offset < 0 || !output) {
                write_failed.store(true);
                throw std::runtime_error("Cannot append NUP shader record");
            }
            impl->entries.emplace(key,
                                  Impl::Entry{job->stage, job->digest, static_cast<u64>(offset),
                                              static_cast<u64>(size), checksum});
        } catch (const std::exception& error) {
            std::scoped_lock lock{writer_mutex};
            impl->pending.push_back({{"archive", job->archive},
                                     {"stage", static_cast<u32>(job->stage)},
                                     {"reason", error.what()}});
        } catch (...) {
            write_failed.store(true);
        }
    };
    const size_t worker_count = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 2, 16);
    Common::ThreadWorker workers(worker_count, "NupShaderBuilder", {},
                                 Common::ThreadPlacement::Background);
    SCOPE_EXIT {
        std::stop_source shutdown;
        shutdown.request_stop();
        workers.WaitForRequests(shutdown.get_token());
    };
    size_t completed = 0;
    for (const auto& member : files) {
        if (stop.stop_requested()) {
            return false;
        }
        progress(completed++, files.size());
        if (member.path.ends_with("ZsDic.pack.zs")) {
            continue;
        }
        try {
            if (member.file->GetSize() > MaxBytes) {
                throw std::runtime_error("Shader archive exceeds memory limit");
            }
            const auto bytes = Decompress(member.file->ReadAllBytes(), dictionaries);
            Shader::Ampere::ReadShaderArchive(bytes, [&](const auto& program) {
                if (stop.stop_requested()) {
                    return false;
                }
                try {
                    const size_t header = program.stage == Shader::Stage::Compute ? 0 : 128;
                    if (program.code.size() < header ||
                        (program.code.size() - header) % sizeof(Instruction)) {
                        throw std::runtime_error("Invalid native code allocation");
                    }
                    const size_t count =
                        std::min((program.code.size() - header) / sizeof(Instruction),
                                 static_cast<size_t>(
                                     Shader::Ampere::ProgramCaptureState::MaximumInstructions));
                    std::vector<Instruction> native(count);
                    std::memcpy(native.data(), program.code.data() + header,
                                native.size() * sizeof(Instruction));
                    Shader::Ampere::ProgramCaptureState capture;
                    bool complete = false;
                    for (size_t i = 0; i < native.size(); ++i) {
                        const auto result = capture.Observe(native[i], static_cast<u32>(i));
                        if (result == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
                            break;
                        }
                        if (result == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                            native.resize(i + 1);
                            complete = true;
                            break;
                        }
                    }
                    if (!complete) {
                        throw std::runtime_error("Native shader capture is incomplete");
                    }
                    const auto digest = Digest({reinterpret_cast<const u8*>(native.data()),
                                                native.size() * sizeof(Instruction)});
                    if (!seen.emplace(static_cast<u32>(program.stage), digest).second) {
                        return true;
                    }
                    auto job = std::make_shared<Job>(
                        Job{program.stage, std::move(native), member.path, digest});
                    if (!workers.TryQueueWork([&, job] { compile(job); })) {
                        compile(job);
                    }
                } catch (const std::exception& error) {
                    std::scoped_lock lock{writer_mutex};
                    impl->pending.push_back({{"archive", member.path},
                                             {"stage", static_cast<u32>(program.stage)},
                                             {"reason", error.what()}});
                }
                return true;
            });
        } catch (const std::exception& error) {
            std::scoped_lock lock{writer_mutex};
            impl->pending.push_back({{"archive", member.path}, {"reason", error.what()}});
        }
    }
    workers.WaitForRequests(stop);
    if (stop.stop_requested()) {
        return false;
    }
    if (source_identity() != identity) {
        impl = std::make_unique<Impl>();
        throw std::runtime_error("NUP changed during shader preloading");
    }
    if (write_failed.load()) {
        throw std::runtime_error("NUP shader record write failed");
    }
    Json programs = Json::array();
    for (const auto& [key, entry] : impl->entries) {
        programs.push_back(
            {{"key", key},
             {"stage", static_cast<u32>(entry.stage)},
             {"native_digest", Json::binary(std::vector<u8>(entry.native_digest.begin(),
                                                            entry.native_digest.end()))},
             {"offset", entry.offset},
             {"size", entry.size},
             {"checksum",
              Json::binary(std::vector<u8>(entry.checksum.begin(), entry.checksum.end()))}});
    }
    Json index{
        {"identity", identity}, {"programs", std::move(programs)}, {"pending", impl->pending}};
    const auto data = Json::to_cbor(index);
    if (data.size() > MaxIndexBytes) {
        throw std::runtime_error("NUP shader index exceeds size limit");
    }
    const auto index_position = output.tellp();
    if (index_position < 0) {
        throw std::runtime_error("Cannot locate NUP shader index");
    }
    const u64 index_offset = static_cast<u64>(index_position);
    const u64 index_size = data.size();
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    std::copy(CacheMagic.begin(), CacheMagic.end(), header.begin());
    std::memcpy(header.data() + 8, &index_offset, sizeof(index_offset));
    std::memcpy(header.data() + 16, &index_size, sizeof(index_size));
    const auto digest = Digest(data);
    std::copy(digest.begin(), digest.end(), header.begin() + 24);
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot save NUP shader cache");
    }
    std::filesystem::rename(temporary, cache_path);
    impl->path = cache_path;
    impl->header = header;
    impl->index_offset = index_offset;
    impl->native_end = index_offset;
    impl->index = std::move(index);
    progress(files.size(), files.size());
    LOG_INFO(Shader,
             "NUP shader preload: compiled {} lowered programs with {} workers, {} pending runtime "
             "context",
             impl->entries.size(), worker_count, impl->pending.size());
    return true;
} catch (const std::exception& error) {
    LOG_WARNING(Shader, "NUP shader preload: {}; continuing with runtime compilation",
                error.what());
    return false;
}

} // namespace VideoCommon
