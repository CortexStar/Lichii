// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "common/bit_field.h"
#include "video_core/pipeline_cache_policy.h"

using namespace VideoCommon;

TEST_CASE("Vertex attribute mask follows the header's input map", "[video_core][pipeline_cache]") {
    std::array<std::array<bool, 4>, 32> inputs{};
    CHECK(VertexAttributeMask(inputs) == 0);

    inputs[0] = {true, false, false, false};
    inputs[3] = {false, false, false, true};
    inputs[31] = {false, true, true, false};
    CHECK(VertexAttributeMask(inputs) == ((u64{3} << 0) | (u64{3} << 6) | (u64{3} << 62)));

    inputs.fill({true, true, true, true});
    CHECK(VertexAttributeMask(inputs) == ~u64{0});
}

TEST_CASE("Pipeline keys keep only the attribute types active stages read",
          "[video_core][pipeline_cache]") {
    constexpr u64 types = 0x1234'5678'9abc'def0;
    constexpr u64 first_four = 0xff;

    SECTION("a VertexA program keeps every type") {
        const std::array<u64, 6> hashes{1, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, 0, 0, 0, 0, 0};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == types);
    }
    SECTION("the vertex program's input map masks the types") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, first_four, 0, 0, 0, ~u64{0}};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == (types & first_four));
    }
    SECTION("inactive stages do not mask") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 0};
        const std::array<u64, 6> masks{0, ~u64{0}, 0, 0, 0, 0};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == types);
    }
    SECTION("task and mesh programs drop every type") {
        const std::array<u64, 6> hashes{0, 0, 7, 8, 0, 3};
        const std::array<u64, 6> masks{0, 0, 0, 0, 0, ~u64{0}};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == 0);
    }
    SECTION("garbage in unread registers no longer changes the key") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, first_four, 0, 0, 0, ~u64{0}};
        const u64 other = (types & first_four) | 0xabcd'0000'0000'0000;
        CHECK(KeyedAttributeTypes(types, hashes, masks) ==
              KeyedAttributeTypes(other, hashes, masks));
    }
}

TEST_CASE("Static vertex input keys keep only what the vertex program reads",
          "[video_core][pipeline_cache]") {
    // FixedPipelineState::VertexAttribute's layout.
    union Attribute {
        u32 raw;
        BitField<0, 1, u32> enabled;
        BitField<1, 5, u32> buffer;
        BitField<6, 14, u32> offset;
    };
    const auto make = [](u32 buffer, u32 offset, bool enabled) {
        Attribute attribute{};
        attribute.enabled.Assign(enabled ? 1 : 0);
        attribute.buffer.Assign(buffer);
        attribute.offset.Assign(offset);
        return attribute;
    };

    SECTION("the keyed attributes come from the vertex program's input map") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, 0b11'0000'11, 0, 0, 0, ~u64{0}};
        CHECK(KeyedStaticAttributes(hashes, masks) == 0b1001);
        const std::array<u64, 6> with_vertex_a{1, 2, 0, 0, 0, 3};
        CHECK(KeyedStaticAttributes(with_vertex_a, masks) == ~u32{0});
        const std::array<u64, 6> mesh{0, 0, 7, 8, 0, 3};
        const std::array<u64, 6> mesh_masks{0, 0, 0, 0, 0, ~u64{0}};
        CHECK(KeyedStaticAttributes(mesh, mesh_masks) == 0);
    }
    SECTION("unread attributes and unused bindings are cleared") {
        std::array<Attribute, 4> attributes{make(0, 0, true), make(1, 12, true), make(2, 4, true),
                                            make(3, 8, false)};
        std::array<u32, 4> divisors{0, 1, 2, 3};
        // Attributes 0 and 3 are read; 3 is a constant, so binding 3 is not fetched.
        MaskStaticVertexInput(attributes, divisors, 0b1001);
        CHECK(attributes[0].raw == make(0, 0, true).raw);
        CHECK(attributes[1].raw == 0);
        CHECK(attributes[2].raw == 0);
        CHECK(attributes[3].raw == make(3, 8, false).raw);
        CHECK(divisors == std::array<u32, 4>{0, 0, 0, 0});
    }
    SECTION("garbage in unread registers no longer changes the key") {
        std::array<Attribute, 4> a{make(0, 0, true), make(1, 12, true), {}, {}};
        std::array<Attribute, 4> b{make(0, 0, true), make(2, 999, true), make(3, 3, true), {}};
        std::array<u32, 4> divisors_a{0, 1, 0, 0};
        std::array<u32, 4> divisors_b{0, 0, 5, 6};
        MaskStaticVertexInput(a, divisors_a, 0b1);
        MaskStaticVertexInput(b, divisors_b, 0b1);
        for (size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].raw == b[i].raw);
        }
        CHECK(divisors_a == divisors_b);
    }
    SECTION("an instanced binding a read attribute fetches keeps its divisor") {
        std::array<Attribute, 2> attributes{make(1, 0, true), make(0, 0, true)};
        std::array<u32, 2> divisors{7, 9};
        MaskStaticVertexInput(attributes, divisors, 0b01);
        CHECK(divisors == std::array<u32, 2>{0, 9});
    }
}
