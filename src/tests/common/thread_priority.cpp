// SPDX-License-Identifier: GPL-3.0-or-later

#include <future>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/thread.h"

#ifdef __linux__
#include <sys/resource.h>
#include <unistd.h>

TEST_CASE("SetOtherThreadPriority changes the target thread, not the caller",
          "[common][thread_priority]") {
    const int caller_nice = getpriority(PRIO_PROCESS, 0);
    std::promise<Common::ThreadPriorityTarget> target;
    std::promise<void> lowered;
    int other_nice{};
    std::thread other{[&] {
        target.set_value(Common::CurrentThreadPriorityTarget());
        lowered.get_future().wait();
        other_nice = getpriority(PRIO_PROCESS, 0);
    }};
    Common::SetOtherThreadPriority(target.get_future().get(), Common::ThreadPriority::Low);
    lowered.set_value();
    other.join();
    // Low is nice 10. Lowering a priority needs no privileges; a runner already above nice 10
    // cannot go back down to it.
    REQUIRE(other_nice == (caller_nice > 10 ? caller_nice : 10));
    REQUIRE(getpriority(PRIO_PROCESS, 0) == caller_nice);
}
#endif

TEST_CASE("Thread priority targets tell threads apart", "[common][thread_priority]") {
    const Common::ThreadPriorityTarget self = Common::CurrentThreadPriorityTarget();
    const Common::ThreadPriorityTarget other =
        std::async(std::launch::async, [] { return Common::CurrentThreadPriorityTarget(); }).get();
    REQUIRE(self.native == Common::CurrentThreadPriorityTarget().native);
    REQUIRE(self.native != other.native);
}
