// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/precache_journal.h"

namespace {
struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("precache-journal-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};
} // Anonymous namespace

TEST_CASE("A build left in flight is skipped by the next precache", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "precache-jobs";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 4, "pipeline"};
        REQUIRE_FALSE(journal.Poisoned(42));
        journal.Begin(2, 42); // the process ends here (a driver crash): no End
        journal.Begin(1, 7);
        journal.End(1);
    }
    Vulkan::PrecacheJournal next{base, "driver 1", 4, "pipeline"};
    REQUIRE(next.Poisoned(42));
    REQUIRE_FALSE(next.Poisoned(7));
    REQUIRE(next.PoisonedCount() == 1);
    // Still skipped at the start after (the poison list is kept, the in-flight slots reset).
    Vulkan::PrecacheJournal third{base, "driver 1", 4, "pipeline"};
    REQUIRE(third.Poisoned(42));
}

TEST_CASE("A new driver tries the skipped builds again", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "warm-graphics.bin";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 2, "graphics program"};
        journal.Begin(0, 9);
    }
    REQUIRE(Vulkan::PrecacheJournal{base, "driver 1", 2, "graphics program"}.Poisoned(9));
    REQUIRE_FALSE(Vulkan::PrecacheJournal{base, "driver 2", 2, "graphics program"}.Poisoned(9));
}

TEST_CASE("Builds in flight with one driver are not skipped by the next",
          "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "precache-jobs";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 2, "pipeline"};
        journal.Begin(0, 5); // ended by a kill or crash, then the driver is updated
    }
    // Recovered first by the new driver: its in-flight ids belong to driver 1.
    Vulkan::PrecacheJournal updated{base, "driver 2", 2, "pipeline"};
    REQUIRE_FALSE(updated.Poisoned(5));
    REQUIRE(updated.PoisonedCount() == 0);
    // Nor are they saved under driver 2 for later starts.
    REQUIRE_FALSE(Vulkan::PrecacheJournal{base, "driver 2", 2, "pipeline"}.Poisoned(5));
}

TEST_CASE("In-flight ids survive a poison list that cannot be saved", "[video_core][precache]") {
    TempDir temp;
    const auto base = temp.path / "precache-jobs";
    {
        Vulkan::PrecacheJournal journal{base, "driver 1", 2, "pipeline"};
        journal.Begin(1, 77);
    }
    auto poison = base;
    poison += ".poison";
    std::filesystem::remove(poison);
    std::filesystem::create_directory(poison); // the new list cannot replace it
    {
        Vulkan::PrecacheJournal blocked{base, "driver 1", 2, "pipeline"};
        REQUIRE(blocked.Poisoned(77)); // still skipped by this precache
        blocked.Begin(0, 78);          // the journal is off: not recorded
    }
    std::filesystem::remove(poison);
    // The in-flight file was not reset, so the next start still names 77.
    Vulkan::PrecacheJournal next{base, "driver 1", 2, "pipeline"};
    REQUIRE(next.Poisoned(77));
    REQUIRE_FALSE(next.Poisoned(78));
    auto tmp = poison;
    tmp += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(tmp));
}
