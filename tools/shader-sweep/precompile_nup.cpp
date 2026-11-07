// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <filesystem>
#include <iostream>
#include <set>
#include <string_view>

#include <openssl/sha.h>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "shader_recompiler/offline_environment.h"
#include "shader_recompiler/offline_graphics.h"
#include "video_core/nup_shader_cache.h"

namespace {
int Run(const std::filesystem::path &path, bool inspect_compute,
        bool inspect_graphics, bool inspect_vertices) {
  Common::Log::Initialize();
  Common::Log::SetColorConsoleBackendEnabled(true);
  Common::Log::Start();
  FileSys::RealVfsFilesystem filesystem;
  const auto utf8 = path.u8string();
  const auto file = filesystem.OpenFile(std::string(utf8.begin(), utf8.end()),
                                        FileSys::OpenMode::Read);
  VideoCommon::NupShaderCache cache;
  const bool prepared =
      cache.Prepare(file, path, {}, [](size_t value, size_t total) {
        std::cout << "Preparing NUP shader archives " << value << '/' << total
                  << '\n';
      });
  if (prepared && inspect_vertices) {
    if (!cache.PrepareVertexLayouts({}, [](size_t done, size_t total) {
          if (done == total)
            std::cout << "Model scan complete " << done << '/' << total << '\n';
        }))
      return 1;
    size_t candidates = 0;
    std::set<VideoCommon::NupShaderCache::VertexInputTypes> printed;
    if (!cache.ForEachGraphicsGroupWithVertexLayouts(
            {}, [&](auto, auto inputs) {
              for (const auto &types : inputs) {
                ++candidates;
                if (!printed.insert(types).second)
                  continue;
                std::cout << "Known vertex types:";
                for (size_t index = 0; index < types.size(); ++index) {
                  if (types[index])
                    std::cout << ' ' << index << ':' << unsigned(types[index]);
                }
                std::cout << '\n';
              }
            }))
      return 1;
    std::cout << "NUP material vertex candidates: " << candidates << '\n';
  }
  if (prepared && inspect_compute) {
    size_t variants = 0, accepted = 0, unique_accepted = 0;
    std::set<std::array<u8, 64>> unique;
    cache.ForEachComputeVariant(
        {}, [&](std::span<const u8> code, std::span<const u8> control,
                const Shader::Ampere::LoweredProgram *lowered) {
          ++variants;
          std::array<u8, 64> identity{};
          SHA256(code.data(), code.size(), identity.data());
          SHA256(control.data(), control.size(), identity.data() + 32);
          const bool first = unique.insert(identity).second;
          if (!lowered)
            return;
          try {
            Shader::OfflineEnvironment environment{code, control, *lowered};
            ++accepted;
            if (first)
              ++unique_accepted;
          } catch (const std::exception &) {
          }
        });
    std::cout << "NUP compute metadata: " << accepted << '/' << variants
              << " variants accepted (not a pipeline compilation result)\n";
    std::cout << "Distinct code/control combinations: " << unique_accepted
              << '/' << unique.size() << " metadata-compatible\n";
  }
  if (prepared && inspect_graphics) {
    size_t groups = 0, accepted = 0;
    std::set<std::array<u8, 32>> seen;
    const bool complete = cache.ForEachGraphicsGroup(
        {}, [&](std::span<const Shader::Ampere::ArchiveProgram> group) {
          std::array<u8, 9 * 64> bytes{};
          for (const auto &program : group) {
            const size_t index = static_cast<size_t>(program.stage);
            SHA256(program.code.data(), program.code.size(),
                   bytes.data() + index * 64);
            SHA256(program.control.data(), program.control.size(),
                   bytes.data() + index * 64 + 32);
          }
          std::array<u8, 32> identity;
          SHA256(bytes.data(), bytes.size(), identity.data());
          if (!seen.insert(identity).second)
            return;
          ++groups;
          try {
            const auto layout = Shader::GetOfflineGraphicsLayout(group);
            for (size_t index = 0; index < layout.count; ++index) {
              const auto &program = *layout.programs[index];
              const auto native = Shader::CaptureOfflineGraphics(program);
              if (!cache.Find(native, program.stage, false))
                throw std::runtime_error("No cached lowering");
            }
            ++accepted;
          } catch (const std::exception &) {
          }
        });
    if (!complete) {
      std::cerr << "NUP graphics enumeration did not complete\n";
      return 1;
    }
    std::cout << "NUP graphics groups: " << accepted << '/' << groups
              << " distinct groups ready for prewarm attempts (not driver "
                 "compilation results)\n";
  }
  return prepared ? 0 : 1;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: nup-shader-precompile GAME.nup "
                 "[--compute-info|--graphics-info|--vertex-info]\n";
    return 2;
  }
#ifdef _WIN32
  const bool inspect =
      argc == 3 && std::wstring_view{argv[2]} == L"--compute-info";
  const bool graphics =
      argc == 3 && std::wstring_view{argv[2]} == L"--graphics-info";
  const bool vertices =
      argc == 3 && std::wstring_view{argv[2]} == L"--vertex-info";
#else
  const bool inspect =
      argc == 3 && std::string_view{argv[2]} == "--compute-info";
  const bool graphics =
      argc == 3 && std::string_view{argv[2]} == "--graphics-info";
  const bool vertices =
      argc == 3 && std::string_view{argv[2]} == "--vertex-info";
#endif
  if (argc == 3 && !inspect && !graphics && !vertices)
    return 2;
  return Run(std::filesystem::path{argv[1]}, inspect, graphics, vertices);
}
