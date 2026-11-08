// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/macro_turing.h"
#include "video_core/nx2_list_indirect.h"

namespace {

namespace LI = Tegra::Nx2ListIndirect;
using Effects = Tegra::TuringMacroInterpreter::Effects;

/// A Turing MME instruction from its fields (TuringMacroInterpreter::Decode); a lane left alone
/// adds the zero register to itself.
struct Mme {
    bool end{};
    u32 pred_mode{};
    u32 pred_reg{};
    std::array<u32, 2> op{}, dst{24, 24}, src_a{24, 24}, src_b{24, 24}, imm{};
    std::array<u32, 2> out_method{}, out_emit{};
};

std::vector<u32> Code(std::span<const Mme> instructions) {
    std::vector<u32> code;
    for (const Mme& f : instructions) {
        std::array<u32, 3> bits{};
        const auto field = [&bits](u32 offset, u32 count, u32 value) {
            for (u32 bit = 0; bit < count; ++bit) {
                bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
            }
        };
        field(0, 1, f.end ? 1 : 0);
        field(1, 4, f.pred_mode);
        field(5, 5, f.pred_reg);
        for (u32 lane = 0; lane < 2; ++lane) {
            field(10 + lane * 36, 5, f.op[lane]);
            field(15 + lane * 36, 5, f.dst[lane]);
            field(20 + lane * 36, 5, f.src_a[lane]);
            field(25 + lane * 36, 5, f.src_b[lane]);
            field(30 + lane * 36, 16, f.imm[lane]);
            field(82 + lane * 7, 3, f.out_method[lane]);
            field(85 + lane * 7, 4, f.out_emit[lane]);
        }
        code.insert(code.end(), {bits[2], bits[1], bits[0]});
    }
    return code;
}

std::vector<u32> Code(std::initializer_list<Mme> instructions) {
    return Code(std::span<const Mme>{instructions.begin(), instructions.size()});
}

/// Sends the next parameter to `method` (an immediate with its increment in bits 12-15).
Mme Fifo(u32 method, bool end = false) {
    return Mme{.end = end, .imm = {method, 0}, .out_method = {5, 0}, .out_emit = {3, 0}};
}

/// Writes `value` (16 bits) to `method`.
Mme Constant(u32 method, u32 value, bool end = false) {
    return Mme{.end = end, .imm = {method, value}, .out_method = {5, 0}, .out_emit = {6, 0}};
}

/// Stores the next parameter in data RAM word `word` (DWRITE).
Mme StoreParameter(u32 word, bool end = false) {
    return Mme{.end = end, .op = {31, 0}, .src_a = {25, 24}, .src_b = {28, 24}, .imm = {word, 0}};
}

/// Writes data RAM word `word` (DREAD on lane 1) to `method`.
Mme EmitDataRam(u32 method, u32 word, bool end = false) {
    return Mme{.end = end,
               .op = {0, 30},
               .src_a = {24, 25},
               .imm = {method, word},
               .out_method = {5, 0},
               .out_emit = {2, 0}};
}

Effects Analyze(std::initializer_list<Mme> instructions, u32 start = 0) {
    return Tegra::TuringMacroInterpreter::AnalyzeEffects(Code(instructions), start);
}

/// A random program as the effects test builds them: registers 0-3 set first, then ALU,
/// register read, flow and data RAM instructions with methods from immediates mostly.
template <typename Pick>
std::vector<Mme> RandomProgram(Pick&& pick, u32 length) {
    std::vector<Mme> instructions(2);
    for (u32 i = 0; i < 4; ++i) {
        instructions[i / 2].dst[i % 2] = i;
        instructions[i / 2].src_a[i % 2] = pick(2) != 0 ? 25 : 28 + i % 2;
        instructions[i / 2].imm[i % 2] = pick(4);
    }
    for (u32 pc = 0; pc < length; ++pc) {
        Mme f{.end = pick(10) == 0};
        if (pick(6) == 0) {
            f.pred_mode = pick(16);
            f.pred_reg = pick(5) == 0 ? 24 : pick(4);
        }
        for (u32 lane = 0; lane < 2; ++lane) {
            constexpr std::array<u32, 13> ops{0, 2, 9, 12, 14, 16, 17, 22, 23, 24, 25, 30, 31};
            f.op[lane] = ops[pick(static_cast<u32>(ops.size()))];
            f.dst[lane] = pick(3) == 0 ? 24 : pick(4);
            f.src_a[lane] = pick(2) != 0 ? pick(4) : 24 + pick(6);
            f.src_b[lane] = pick(2) != 0 ? pick(4) : 24 + pick(6);
            if (f.op[lane] == 23) {
                f.imm[lane] = 2 + pick(4);
            } else if (f.op[lane] == 24 || f.op[lane] == 25) {
                // Short jumps and branches either way, some stopping the call.
                const int offset = pick(8) == 0 ? -4096 : static_cast<int>(pick(9)) - 4;
                f.imm[lane] =
                    (static_cast<u32>(offset) & 0x1fff) | (pick(2) << 15) | (pick(2) << 14);
            } else if (f.op[lane] == 30 || f.op[lane] == 31) {
                f.src_a[lane] = 25; // A data RAM word in range
                f.imm[lane] = pick(16);
            } else {
                f.imm[lane] = pick(3) == 0 ? 0x100 + pick(8) + (pick(3) << 12) : pick(0x10000);
            }
            f.out_method[lane] = pick(3) == 0 ? 0 : (pick(4) == 0 ? 1 + pick(4) : 5 + lane);
            f.out_emit[lane] = pick(3) == 0 ? 0 : (pick(3) == 0 ? 1 + pick(2) : 3 + lane);
        }
        instructions.push_back(f);
    }
    return instructions;
}

struct Executed {
    bool success{};
    std::vector<std::pair<u32, u32>> emits;
    Tegra::TuringMacroInterpreter::DataRam data_ram{};
};

Executed Execute(std::span<const u32> code, u32 start, std::vector<u32> params,
                 const Tegra::TuringMacroInterpreter::DataRam& data_ram,
                 std::span<const u32> registers) {
    Tegra::TuringMacroInterpreter interpreter;
    Executed out;
    out.data_ram = data_ram;
    interpreter.active_dram = &out.data_ram;
    const auto result = interpreter.Execute(
        code, start, params,
        [&](u32 address) {
            // The engine's registers hold what the call emitted (as the front's FrontApply).
            for (auto it = out.emits.rbegin(); it != out.emits.rend(); ++it) {
                if (it->first == address) {
                    return it->second;
                }
            }
            return address < registers.size() ? registers[address] : 0U;
        },
        [&](u32 method, u32 value) {
            // The engines' emit refuses macro methods (KeplerCompute::FrontRunMacro).
            if (method >= 0xe00) {
                return false;
            }
            out.emits.emplace_back(method, value);
            return out.emits.size() < 512;
        });
    out.success = result.success;
    return out;
}

/// The front's view of one compute engine for ComputeFront, over plain arrays.
struct MockEngine {
    std::vector<u32> code;
    std::array<u32, 0xc00> data_ram{};
    std::array<u32, 0x1000> registers{};
    LI::UploadState upload;
    std::vector<std::pair<u32, u32>> emits;
    std::vector<std::pair<u64, u32>> owed;
    std::vector<u32> released;
    std::vector<u32> slot_reads;
    /// The words the host GPU's slot 5 holds.
    std::array<u32, 3> slot_words{7, 8, 9};

    struct Context {
        MockEngine& engine;
        std::span<const u32> code;
        u64 generation;
        u32 start;
        std::vector<u32>& params;
        std::span<u32> data_ram;
        std::span<const u32> registers;
        LI::UploadState upload;

        [[nodiscard]] std::optional<u64> DeviceWord(u64 gpu_address) const {
            return gpu_address + 0x10'0000;
        }
        void Emit(u32 method, u32 value) const {
            engine.emits.emplace_back(method, value);
            LI::NoteUploadMethod(engine.upload, method, value,
                                 [this](u32 reg) { return engine.registers[reg]; });
            engine.registers[method] = value;
        }
        void SendOwed(u64 device, u32 value) const {
            engine.owed.emplace_back(device, value);
        }
        void Release(u32 slot) const {
            engine.released.push_back(slot);
        }
        void ReadSlots(std::span<const u32> slots, std::span<u32> words) const {
            for (size_t i = 0; i < slots.size(); ++i) {
                engine.slot_reads.push_back(slots[i]);
                for (u32 k = 0; k < 3; ++k) {
                    words[i * 3 + k] = slots[i] == 5 ? engine.slot_words[k] : 0;
                }
            }
        }
        void SetRegister(u32 method, u32 value) const {
            engine.registers[method] = value;
        }
    };

    Context Call(u32 start, std::vector<u32>& params) {
        return Context{*this, code, 1, start, params, std::span<u32>{data_ram}, registers, upload};
    }
};

} // Anonymous namespace

TEST_CASE("Turing macro effects: a FIFO read writes its registers, up to the one after END",
          "[video_core][macro]") {
    // Ocarina of Time's render enable macro reads six words at the address in its parameters.
    const auto effects =
        Analyze({Fifo(0x154), Fifo(0x155), Fifo(0x158, true), Fifo(0x556), Fifo(0x557)});
    CHECK(effects.methods_known);
    CHECK_FALSE(effects.data_ram);
    CHECK(effects.methods == std::vector<u32>{0x154, 0x155, 0x158, 0x556});
    // The same code entered past its END: the call runs to the end of the code.
    CHECK(Analyze({Fifo(0x154), Fifo(0x155), Fifo(0x158, true), Fifo(0x556), Fifo(0x557)}, 3)
              .methods == std::vector<u32>{0x556, 0x557});
}

TEST_CASE("Turing macro effects: the data RAM words and registers a macro reads",
          "[video_core][macro]") {
    const auto reads = Analyze({EmitDataRam(0x6d, 10), EmitDataRam(0x6d, 12, true), Mme{}});
    CHECK(reads.data_ram);
    CHECK(reads.data_ram_reads_known);
    CHECK(reads.data_ram_reads == std::vector<u32>{10, 12});
    // A register read (MME_READ, op 22) at a constant address.
    const Mme read{.end = true, .op = {22, 0}, .dst = {0, 24}, .src_a = {25, 24}, .imm = {0x44, 0}};
    const auto registers = Analyze({read, Mme{}});
    CHECK(registers.register_reads_known);
    CHECK(registers.register_reads == std::vector<u32>{0x44});
    // A data RAM address from a parameter: any word.
    const Mme dread{.end = true, .op = {30, 0}, .dst = {0, 24}, .src_a = {28, 24}};
    CHECK_FALSE(Analyze({dread, Mme{}}).data_ram_reads_known);
}

TEST_CASE("Turing macro effects cover every write and read Execute makes on random code",
          "[video_core][macro]") {
    std::mt19937 rng{7};
    const auto pick = [&rng](u32 count) { return static_cast<u32>(rng() % count); };
    for (u32 program = 0; program < 400; ++program) {
        const u32 length = 2 + pick(20);
        const auto code = Code(RandomProgram(pick, length));
        const u32 start = pick(4) == 0 ? pick(length + 2) : 0;
        const auto effects = Tegra::TuringMacroInterpreter::AnalyzeEffects(code, start);
        for (u32 run = 0; run < 4; ++run) {
            Tegra::TuringMacroInterpreter interpreter;
            std::vector<u32> fifo(4 + pick(28));
            std::ranges::generate(fifo, [&] { return pick(16) == 0 ? rng() : pick(8); });
            std::vector<u32> written;
            std::vector<u32> registers_read;
            interpreter.Execute(
                code, start, fifo,
                [&](u32 address) {
                    registers_read.push_back(address);
                    return pick(8);
                },
                [&](u32 method, u32) {
                    written.push_back(method);
                    return written.size() < 512;
                });
            CAPTURE(program, run);
            if (effects.methods_known) {
                for (const u32 method : written) {
                    CAPTURE(method);
                    CHECK(std::ranges::binary_search(effects.methods, method));
                }
            }
            if (!effects.data_ram) {
                CHECK(interpreter.data_ram_reads.empty());
                CHECK(interpreter.data_ram_writes.empty());
            }
            if (effects.data_ram_reads_known) {
                for (const auto& [word, value] : interpreter.data_ram_reads) {
                    CAPTURE(word);
                    CHECK(std::ranges::binary_search(effects.data_ram_reads, word));
                }
            }
            if (effects.register_reads_known) {
                for (const u32 address : registers_read) {
                    CAPTURE(address);
                    CHECK(std::ranges::binary_search(effects.register_reads, address));
                }
            }
        }
    }
}

TEST_CASE("Nx2ListIndirect: an untagged dry run does what the interpreter does",
          "[video_core][nx2_list_indirect]") {
    std::mt19937 rng{11};
    const auto pick = [&rng](u32 count) { return static_cast<u32>(rng() % count); };
    u32 done = 0;
    for (u32 program = 0; program < 2000; ++program) {
        const u32 length = 2 + pick(20);
        const auto code = Code(RandomProgram(pick, length));
        const u32 start = pick(4) == 0 ? pick(length + 2) : 0;
        std::vector<u32> params(4 + pick(28));
        std::ranges::generate(params, [&] { return pick(16) == 0 ? rng() : pick(8); });
        Tegra::TuringMacroInterpreter::DataRam data_ram{};
        std::ranges::generate(data_ram, [&] { return pick(8); });
        std::array<u32, 0x1000> registers{};
        std::ranges::generate(registers, [&] { return pick(8); });
        const Executed reference = Execute(code, start, params, data_ram, registers);
        const std::vector<u8> untagged(params.size(), 0);
        std::vector<Tegra::TuringMacroInterpreter::Decoded> decoded;
        LI::TaggedRun run;
        LI::RunTagged(
            code, start, params, untagged,
            [&](u32 word) { return std::pair<u32, bool>{data_ram[word], false}; },
            [&](u32 address) {
                return std::pair<u32, bool>{address < registers.size() ? registers[address] : 0U,
                                            false};
            },
            decoded, run);
        CAPTURE(program);
        // Nothing tagged never escapes; a call the interpreter fails, the dry run fails too.
        REQUIRE(run.status != LI::TaggedRun::Outcome::Escaped);
        if (reference.emits.size() >= 512) {
            continue; // The interpreter's emit callback stopped it
        }
        CAPTURE(run.why, static_cast<int>(run.status), reference.emits.size());
        REQUIRE((run.status == LI::TaggedRun::Outcome::Done) == reference.success);
        if (!reference.success) {
            continue;
        }
        ++done;
        REQUIRE(run.emits.size() == reference.emits.size());
        for (size_t i = 0; i < run.emits.size(); ++i) {
            CHECK(run.emits[i].address == reference.emits[i].first);
            CHECK(run.emits[i].value == reference.emits[i].second);
            CHECK_FALSE(run.emits[i].tagged);
        }
        auto stored = data_ram;
        for (const LI::TaggedWrite& store : run.data_ram) {
            stored[store.address] = store.value;
        }
        CHECK(stored == reference.data_ram);
    }
    CHECK(done > 100);
}

TEST_CASE("Nx2ListIndirect: a tagged dry run that ends puts the words where the interpreter does",
          "[video_core][nx2_list_indirect]") {
    // Every Done run with tagged parameters must take the same path whatever the words are: run
    // the interpreter with two different sets of words and compare with the placeholders.
    std::mt19937 rng{13};
    const auto pick = [&rng](u32 count) { return static_cast<u32>(rng() % count); };
    u32 done = 0;
    u32 tagged_emits = 0;
    for (u32 program = 0; program < 20000; ++program) {
        const u32 length = 2 + pick(12);
        const auto code = Code(RandomProgram(pick, length));
        const u32 count = 1 + pick(3);
        std::vector<u32> params(count + pick(4));
        std::ranges::generate(params, [&] { return pick(8); });
        std::vector<u8> tags(params.size(), 0);
        for (u32 k = 0; k < count; ++k) {
            params[k] = LI::Placeholder(5, k);
            tags[k] = 1;
        }
        Tegra::TuringMacroInterpreter::DataRam data_ram{};
        std::ranges::generate(data_ram, [&] { return pick(8); });
        std::array<u32, 0x1000> registers{};
        std::vector<Tegra::TuringMacroInterpreter::Decoded> decoded;
        LI::TaggedRun run;
        LI::RunTagged(
            code, 0, params, tags,
            [&](u32 word) { return std::pair<u32, bool>{data_ram[word], false}; },
            [&](u32 address) { return std::pair<u32, bool>{registers[address & 0xfff], false}; },
            decoded, run);
        if (run.status != LI::TaggedRun::Outcome::Done) {
            continue;
        }
        CAPTURE(program);
        ++done;
        for (const std::array<u32, 3> words :
             {std::array<u32, 3>{0x1234, 0x22, 0x3}, std::array<u32, 3>{0x7, 0xffff, 0x4242}}) {
            std::vector<u32> concrete = params;
            for (u32 k = 0; k < count; ++k) {
                concrete[k] = words[k];
            }
            const auto word_of = [&](u32 value, bool tagged) {
                return tagged ? words[LI::PlaceholderWord(value)] : value;
            };
            const Executed reference = Execute(code, 0, concrete, data_ram, registers);
            if (reference.emits.size() >= 512) {
                continue; // The interpreter's emit callback stopped it
            }
            REQUIRE(reference.success);
            REQUIRE(run.emits.size() == reference.emits.size());
            for (size_t i = 0; i < run.emits.size(); ++i) {
                CHECK(run.emits[i].address == reference.emits[i].first);
                CHECK(word_of(run.emits[i].value, run.emits[i].tagged) ==
                      reference.emits[i].second);
                tagged_emits += run.emits[i].tagged ? 1 : 0;
            }
            auto stored = data_ram;
            for (const LI::TaggedWrite& store : run.data_ram) {
                stored[store.address] = word_of(store.value, store.tagged);
            }
            CHECK(stored == reference.data_ram);
        }
    }
    CHECK(done > 100);
    CHECK(tagged_emits > 0);
}

TEST_CASE("Nx2ListIndirect: compute list words reach an inline upload and a QMD grid untouched",
          "[video_core][nx2_list_indirect]") {
    // Ocarina of Time's culling flow: 0xe74 stores the three list words in the data RAM, 0xe7a
    // uploads them as constant buffer words 1-3, 0xe76 writes them as the inline QMD's grid.
    MockEngine engine;
    engine.code = Code({
        // 0: the store macro
        StoreParameter(10),
        StoreParameter(11),
        StoreParameter(12, true),
        Mme{},
        // 4: the upload macro (a linear upload of one line of 16 bytes at 0x2000)
        Constant(LI::UploadLineLength, 16),
        Constant(LI::UploadLineCount, 1),
        Constant(LI::UploadAddressHigh, 0),
        Constant(LI::UploadAddressLow, 0x2000),
        Constant(LI::UploadExec, 1),
        Constant(LI::UploadData, 0x55),
        EmitDataRam(LI::UploadData, 10),
        EmitDataRam(LI::UploadData, 11),
        EmitDataRam(LI::UploadData, 12, true),
        Mme{},
        // 14: the launch macro
        EmitDataRam(LI::InlineQmdGridFirst, 10),
        EmitDataRam(LI::InlineQmdGridFirst + 1, 11),
        EmitDataRam(LI::InlineQmdGridFirst + 2, 12, true),
        Mme{},
        // 18: a macro that branches on a data RAM word (as a 3D macro or a check might)
        Mme{.op = {30, 0}, .dst = {0, 24}, .src_a = {25, 24}, .imm = {10, 0}},
        Mme{.op = {25, 0}, .src_a = {0, 24}, .imm = {0x8000 | 2, 0}},
        Constant(0x100, 1, true),
        Mme{},
    });
    static std::array<u32, 0xc00> channel_ram_key{};
    LI::DataRamWords& channel = LI::GetChannel(&channel_ram_key);
    LI::ComputeFront front;

    std::vector<u32> no_params;
    auto prepare = engine.Call(0, no_params);
    REQUIRE(front.PrepareCall(prepare, channel, 5, 3));
    std::vector<u32> params{LI::Placeholder(5, 0), LI::Placeholder(5, 1), LI::Placeholder(5, 2)};
    bool concretized{};
    LI::Reason reason{};
    auto store = engine.Call(0, params);
    REQUIRE(front.Run(store, channel, concretized, reason));
    CHECK(engine.data_ram[10] == LI::Placeholder(5, 0));
    CHECK(engine.data_ram[12] == LI::Placeholder(5, 2));
    CHECK(channel.words.size() == 3);

    std::vector<u32> none;
    auto upload = engine.Call(4, none);
    REQUIRE(front.Run(upload, channel, concretized, reason));
    CHECK_FALSE(concretized);
    // The constant word goes as is, the list words as placeholders, owed at their places.
    REQUIRE(engine.owed.size() == 3);
    CHECK(engine.owed[0] == std::pair<u64, u32>{0x10'2004, LI::Placeholder(5, 0)});
    CHECK(engine.owed[2] == std::pair<u64, u32>{0x10'200c, LI::Placeholder(5, 2)});

    auto launch = engine.Call(14, none);
    REQUIRE(front.Run(launch, channel, concretized, reason));
    CHECK(LI::GridSlot(engine.registers[LI::InlineQmdGridFirst],
                       engine.registers[LI::InlineQmdGridFirst + 1],
                       engine.registers[LI::InlineQmdGridFirst + 2]) == std::optional<u32>{5});
    CHECK(engine.slot_reads.empty());

    // A branch on a list word: the front reads the words back and runs the call on them.
    auto branch = engine.Call(18, none);
    CHECK_FALSE(front.Run(branch, channel, concretized, reason));
    CHECK(concretized);
    CHECK(reason == LI::Reason::Escaped);
    CHECK(engine.slot_reads == std::vector<u32>{5});
    CHECK(engine.data_ram[10] == 7);
    CHECK(engine.data_ram[11] == 8);
    CHECK(engine.data_ram[12] == 9);
    CHECK(channel.words.empty());
    // The registers that held the grid's placeholders got the words too.
    CHECK(engine.registers[LI::InlineQmdGridFirst] == 7);
    CHECK(engine.registers[LI::InlineQmdGridFirst + 2] == 9);
    LI::ForgetChannel(&channel_ram_key);
}

TEST_CASE("Nx2ListIndirect: a list word sent anywhere else is refused before the call takes it",
          "[video_core][nx2_list_indirect]") {
    MockEngine engine;
    // Stores its first parameter and sends the second to a register the back doesn't follow.
    engine.code = Code({StoreParameter(10), Fifo(0x200, true), Mme{}});
    static std::array<u32, 0xc00> channel_ram_key{};
    LI::DataRamWords& channel = LI::GetChannel(&channel_ram_key);
    LI::ComputeFront front;
    std::vector<u32> no_params;
    auto prepare = engine.Call(0, no_params);
    CHECK_FALSE(front.PrepareCall(prepare, channel, 5, 2));
    CHECK(channel.words.empty());
    LI::ForgetChannel(&channel_ram_key);
}

TEST_CASE("Nx2ListIndirect: placeholders and grids", "[video_core][nx2_list_indirect]") {
    for (u32 slot : {0U, 1U, 4095U}) {
        for (u32 word = 0; word < LI::SlotWords; ++word) {
            const u32 value = LI::Placeholder(slot, word);
            CHECK(LI::IsPlaceholder(value));
            CHECK(LI::PlaceholderSlot(value) == slot);
            CHECK(LI::PlaceholderWord(value) == word);
            // Never an empty grid word where the QMD scheduler looks (all of the width, the low
            // 16 bits of the height and depth).
            CHECK((word == 0 ? value : value & 0xffff) != 0);
        }
    }
    CHECK_FALSE(LI::IsPlaceholder(LI::PlaceholderMagic | 3));
    CHECK(LI::GridSlot(LI::Placeholder(9, 0), LI::Placeholder(9, 1), LI::Placeholder(9, 2)) ==
          std::optional<u32>{9});
    CHECK_FALSE(LI::GridSlot(LI::Placeholder(9, 0), LI::Placeholder(8, 1), LI::Placeholder(9, 2)));
    CHECK_FALSE(LI::GridSlot(LI::Placeholder(9, 1), LI::Placeholder(9, 1), LI::Placeholder(9, 2)));
}
