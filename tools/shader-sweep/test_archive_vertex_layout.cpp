// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "shader_recompiler/frontend/ampere/archive_vertex_layout.h"

namespace {
using namespace Shader::Ampere;

void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Bytes {
  std::vector<u8> data;
  explicit Bytes(size_t size) : data(size) {}

  size_t Allocate(size_t size) {
    const size_t start = (data.size() + 7) & ~size_t{7};
    data.resize(start + size);
    return start;
  }
  template <typename T> void Put(size_t offset, T value) {
    Require(offset <= data.size() && sizeof(T) <= data.size() - offset,
            "test fixture overflow");
    for (size_t i = 0; i < sizeof(T); ++i)
      data[offset + i] = static_cast<u8>(u64{value} >> (8 * i));
  }
  void Magic(size_t offset, const char *value) {
    std::memcpy(data.data() + offset, value, 4);
  }
  size_t Name(std::string_view value) {
    const size_t start = Allocate(2 + value.size() + 1);
    Put<u16>(start, static_cast<u16>(value.size()));
    std::memcpy(data.data() + start + 2, value.data(), value.size());
    return start;
  }
  size_t Dictionary(std::initializer_list<const char *> names) {
    const size_t start = Allocate(8 + (names.size() + 1) * 16);
    Put<u32>(start + 4, static_cast<u32>(names.size()));
    size_t index = 1;
    for (const char *name : names)
      Put<u64>(start + 8 + index++ * 16 + 8, Name(name));
    return start;
  }
  void Finish() { Put<u32>(28, static_cast<u32>(data.size())); }
};

struct BfresFixture {
  Bytes bytes{0xf0};
  size_t model, vertices, materials, shapes, first_attribute, first_info, first_values;

  BfresFixture() {
    auto &b = bytes;
    b.Magic(0, "FRES");
    b.Put<u32>(8, 0x000a0400);
    b.Put<u16>(12, 0xfeff);
    model = b.Allocate(0x78);
    vertices = b.Allocate(2 * 0x58);
    materials = b.Allocate(2 * 0xb0);
    shapes = b.Allocate(2 * 0x60);
    b.Put<u64>(40, model);
    b.Put<u64>(48, b.Dictionary({"Object"}));
    b.Put<u16>(220, 1);
    b.Magic(model, "FMDL");
    b.Put<u64>(model + 8, b.Name("Object"));
    b.Put<u64>(model + 32, vertices);
    b.Put<u64>(model + 40, shapes);
    b.Put<u64>(model + 48, b.Dictionary({"First", "Second"}));
    b.Put<u64>(model + 56, materials);
    b.Put<u64>(model + 64, b.Dictionary({"SignedMat", "UnsignedMat"}));
    b.Put<u16>(model + 104, 2);
    b.Put<u16>(model + 106, 2);
    b.Put<u16>(model + 108, 2);
    for (u32 index = 0; index < 2; ++index) {
      const size_t vertex = vertices + index * 0x58;
      b.Magic(vertex, "FVTX");
      const size_t attributes = b.Allocate(32);
      if (index == 0)
        first_attribute = attributes;
      b.Put<u64>(vertex + 8, attributes);
      const char *source = index == 0 ? "unsigned_data" : "signed_data";
      b.Put<u64>(vertex + 16, b.Dictionary({source, "normalized_data"}));
      const size_t stride = b.Allocate(16);
      b.Put<u64>(vertex + 56, stride);
      b.Put<u32>(stride, 4);
      b.Put<u8>(vertex + 76, 2);
      b.Put<u8>(vertex + 77, 1);
      b.Put<u16>(vertex + 78, static_cast<u16>(index));
      b.Put<u32>(vertex + 80, 3);
      b.Put<u64>(attributes, b.Name(source));
      b.Put<u8>(attributes + 8, index == 0 ? 3 : 4);
      b.Put<u8>(attributes + 9, 2); // R8 UInt/SInt
      b.Put<u64>(attributes + 16, b.Name("normalized_data"));
      b.Put<u8>(attributes + 24, 1);
      b.Put<u8>(attributes + 25, 9); // RG8 UNorm
      b.Put<u16>(attributes + 28, 2);

      const size_t material = materials + index * 0xb0;
      b.Magic(material, "FMAT");
      b.Put<u64>(material + 8,
                 b.Name(index == 0 ? "SignedMat" : "UnsignedMat"));
      const size_t info = b.Allocate(80);
      if (index == 0)
        first_info = info;
      const size_t assign = b.Allocate(88);
      b.Put<u64>(material + 16, info);
      b.Put<u64>(info, assign);
      b.Put<u64>(assign, b.Name("Shaders"));
      b.Put<u64>(assign + 8, b.Name("Surface"));
      b.Put<u64>(assign + 48, b.Dictionary({"input_a", "input_b"}));
      const size_t values = b.Allocate(16);
      if (index == 0)
        first_values = values;
      b.Put<u64>(info + 8, values);
      b.Put<u8>(info + 68, 2);
      b.Put<u64>(values, b.Name(index == 0 ? "signed_data" : "unsigned_data"));
      b.Put<u64>(values + 8, b.Name("normalized_data"));

      const size_t shape = shapes + index * 0x60;
      b.Magic(shape, "FSHP");
      b.Put<u64>(shape + 8, b.Name(index == 0 ? "First" : "Second"));
      b.Put<u64>(shape + 16, vertex);
      // Deliberately cross materials: taking material zero for every shape is
      // wrong.
      b.Put<u16>(shape + 82, static_cast<u16>(1 - index));
      b.Put<u16>(shape + 86, static_cast<u16>(index));
    }
    b.Finish();
  }
};

struct BfshaFixture {
  Bytes bytes{0x80};
  size_t model, attributes;
  BfshaFixture() {
    auto &b = bytes;
    b.Magic(0, "FSHA");
    b.Put<u32>(8, 0x00090101);
    b.Put<u16>(12, 0xfeff);
    model = b.Allocate(0x100);
    b.Put<u64>(0x38, b.Name("Shaders"));
    b.Put<u64>(0x48, model);
    b.Put<u64>(0x50, b.Dictionary({"Surface"}));
    b.Put<u16>(0x78, 1);
    b.Put<u64>(model, b.Name("Surface"));
    attributes = b.Allocate(4);
    b.Put<u64>(model + 40, attributes);
    b.Put<u64>(model + 48, b.Dictionary({"input_a", "input_b"}));
    b.Put<u8>(model + 236, 2);
    b.Put<u8>(attributes, 0);
    b.Put<u8>(attributes + 1, 6);
    b.Put<u8>(attributes + 2, 1);
    b.Put<u8>(attributes + 3, 2);
    const size_t binary = b.Allocate(0x98);
    b.Magic(binary, "BNSH");
    b.Put<u32>(binary + 28, 0x98);
    b.Put<u64>(model + 168, binary);
    b.Finish();
  }
};

template <typename F> void Rejects(F &&operation, const char *message = nullptr) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error &error) {
    rejected = true;
    if (message)
      Require(std::string_view{error.what()}.find(message) != std::string_view::npos,
              "Wrong rejection diagnostic");
  }
  Require(rejected, "Malformed or ambiguous input was accepted");
}

void SyntheticTests() {
  const BfresFixture source;
  const BfshaFixture shaders;
  const auto layouts = ReadBfresVertexLayouts(source.bytes.data);
  const auto maps = ReadBfshaVertexAttributeMaps(shaders.bytes.data);
  Require(layouts.size() == 2 && maps.size() == 1, "Fixture records missing");
  Require(std::memcmp(shaders.bytes.data.data() + maps[0].shader_binary_offset,
                      "BNSH", 4) == 0,
          "Shader model lost its embedded binary");
  Require(layouts[0].material_index == 1 && layouts[1].material_index == 0,
          "Shape/material linkage lost");
  const auto first = ResolveArchiveVertexLayout(layouts[0], maps);
  const auto second = ResolveArchiveVertexLayout(layouts[1], maps);
  Require(first.size() == 2 && first[0].location == 2 && first[1].location == 6,
          "Shader locations not resolved");
  Require(first[0].attribute.type == ArchiveVertexType::Float &&
              first[1].attribute.type == ArchiveVertexType::UnsignedInt &&
              second[1].attribute.type == ArchiveVertexType::SignedInt,
          "Vertex formats lost their numeric type");
  Require(first[1].attribute.source_name == "unsigned_data" &&
              second[1].attribute.source_name == "signed_data",
          "Wrong material or vertex buffer selected");

  auto partial = source;
  partial.bytes.Put<u64>(partial.first_values, partial.bytes.Name("absent_source"));
  partial.bytes.Finish();
  const auto partial_layouts = ReadBfresVertexLayouts(partial.bytes.data);
  const auto partial_inputs = ResolveArchiveVertexLayout(partial_layouts[1], maps);
  Require(partial_layouts[0].attributes.size() == 2 &&
              partial_layouts[0].missing_attributes.empty() &&
              partial_layouts[1].attributes.size() == 1 &&
              partial_layouts[1].missing_attributes == std::vector<std::string>{"input_a"} &&
              partial_layouts[1].default_attributes.empty() &&
              partial_inputs.size() == 1 && partial_inputs[0].location == 2 &&
              partial_inputs[0].attribute.type == ArchiveVertexType::Float,
          "Missing source must preserve known attributes without inventing its type");

  auto absent = source;
  std::fill_n(absent.bytes.data.begin() + absent.vertices, 0x58, 0);
  Rejects([&] { (void)ReadBfresVertexLayouts(absent.bytes.data); }, "FVTX metadata absent");

  auto indexed = source;
  const size_t indices = indexed.bytes.Allocate(3);
  indexed.bytes.Put<u64>(indexed.first_info + 16, indices);
  indexed.bytes.Put<u8>(indexed.first_info + 68, 1);
  indexed.bytes.Put<u8>(indices,
                        0); // One used value, followed by the full mapping.
  indexed.bytes.Put<u8>(indices + 1, 0);
  indexed.bytes.Put<u8>(indices + 2, 255);
  indexed.bytes.Finish();
  const auto indexed_layouts = ReadBfresVertexLayouts(indexed.bytes.data);
  Require(indexed_layouts[1].attributes.size() == 1 &&
              indexed_layouts[1].default_attributes ==
                  std::vector<std::string>{"input_b"} &&
              ResolveArchiveVertexLayout(indexed_layouts[1], maps).size() == 1,
          "Default assignment was fabricated into an input binding");
  indexed.bytes.Put<u8>(indices + 1, 1);
  Rejects([&] { (void)ReadBfresVertexLayouts(indexed.bytes.data); });

  auto inactive = shaders;
  inactive.bytes.Put<u8>(inactive.attributes + 1, 255);
  Require(ResolveArchiveVertexLayout(
              layouts[0], ReadBfshaVertexAttributeMaps(inactive.bytes.data))
                  .size() == 1,
          "Inactive shader input was assigned a location");

  auto bad = source;
  bad.bytes.Put<u32>(8, 0x00090000);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u64>(40, std::numeric_limits<u64>::max());
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u16>(bad.shapes + 82, 2);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u64>(bad.shapes + 16, bad.vertices + 0x58);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u16>(bad.first_attribute + 8, 0xffff);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u16>(bad.first_attribute + 12, 4);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u16>(bad.first_attribute + 14, 1);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u8>(bad.first_info + 68, 1);
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  bad = source;
  bad.bytes.Put<u64>(bad.first_attribute, std::numeric_limits<u64>::max());
  Rejects([&] { (void)ReadBfresVertexLayouts(bad.bytes.data); });
  auto bad_shader = shaders;
  bad_shader.bytes.Put<u8>(bad_shader.attributes + 3, 6);
  Rejects([&] { (void)ReadBfshaVertexAttributeMaps(bad_shader.bytes.data); });
  bad_shader = shaders;
  bad_shader.bytes.Put<u8>(bad_shader.attributes + 1, 32);
  Rejects([&] { (void)ReadBfshaVertexAttributeMaps(bad_shader.bytes.data); });
  bad_shader = shaders;
  bad_shader.bytes.Put<u64>(bad_shader.model + 168,
                            std::numeric_limits<u64>::max());
  Rejects([&] { (void)ReadBfshaVertexAttributeMaps(bad_shader.bytes.data); });
  auto bad_maps = maps;
  bad_maps[0].attributes.push_back(bad_maps[0].attributes[0]);
  Rejects([&] { (void)ResolveArchiveVertexLayout(layouts[0], bad_maps); });
  auto duplicate_maps = maps;
  duplicate_maps.push_back(maps.front());
  Rejects(
      [&] { (void)ResolveArchiveVertexLayout(layouts[0], duplicate_maps); });
  auto wrong = layouts.front();
  wrong.shader_archive = "Other";
  Rejects([&] { (void)ResolveArchiveVertexLayout(wrong, maps); });
  wrong = layouts.front();
  wrong.attributes[0].shader_name = "missing";
  Rejects([&] { (void)ResolveArchiveVertexLayout(wrong, maps); });
  auto truncated = source.bytes.data;
  truncated.resize(source.shapes + 1);
  const u32 size = static_cast<u32>(truncated.size());
  for (size_t i = 0; i < 4; ++i)
    truncated[28 + i] = static_cast<u8>(size >> (i * 8));
  Rejects([&] { (void)ReadBfresVertexLayouts(truncated); });
}

std::vector<u8> ReadFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  const auto size = file.tellg();
  Require(size >= 4 && size <= 512 * 1024 * 1024, "Invalid sample size");
  std::vector<u8> data(static_cast<size_t>(size));
  file.seekg(0);
  Require(bool(file.read(reinterpret_cast<char *>(data.data()), size)),
          "Cannot read sample");
  return data;
}

void WriteFixtures(const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  const auto write = [&](const char *name, const std::vector<u8> &data) {
    const auto path = directory / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Require(bool(file), "Cannot create fixture");
    file.write(reinterpret_cast<const char *>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.close();
    Require(bool(file), "Cannot write fixture");
    Require(ReadFile(path) == data, "Fixture roundtrip mismatch");
    std::cout << "Wrote " << path.string() << '\n';
  };
  write("synthetic_cross_materials.bfres", BfresFixture{}.bytes.data);
  write("synthetic_shader_locations.bfsha", BfshaFixture{}.bytes.data);
  std::cout << "Metadata fixtures only: embedded BNSH is a parser skeleton, "
               "not a compilable native shader\n";
}
} // namespace

int main(int argc, char **argv) try {
  SyntheticTests();
  std::cout << "Synthetic archive vertex-layout tests passed\n";
  std::vector<ArchiveVertexLayout> layouts;
  std::vector<ArchiveShaderVertexMap> maps;
  for (int i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--write-fixtures") {
      Require(i + 1 < argc, "--write-fixtures requires a directory");
      WriteFixtures(std::filesystem::path{
          std::u8string{reinterpret_cast<const char8_t *>(argv[++i])}});
      continue;
    }
    const auto data = ReadFile(std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t *>(argv[i])}});
    if (std::memcmp(data.data(), "FRES", 4) == 0) {
      auto sample = ReadBfresVertexLayouts(data);
      size_t known = 0, missing = 0;
      for (const auto &layout : sample) {
        known += layout.attributes.size();
        missing += layout.missing_attributes.size();
      }
      std::cout << argv[i] << ": " << sample.size() << " linked shapes, " << known
                << " known attributes, " << missing << " missing assignments\n";
      layouts.insert(layouts.end(), sample.begin(), sample.end());
    } else {
      auto sample = ReadBfshaVertexAttributeMaps(data);
      std::cout << argv[i] << ": " << sample.size() << " shader models\n";
      maps.insert(maps.end(), sample.begin(), sample.end());
    }
  }
  if (!maps.empty()) {
    size_t resolved = 0;
    for (const auto &layout : layouts)
      resolved += ResolveArchiveVertexLayout(layout, maps).size();
    std::cout << resolved << " explicit vertex-input bindings resolved\n";
  }
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
