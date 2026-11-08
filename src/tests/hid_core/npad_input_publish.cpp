// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/resources/npad/npad_input_publish.h"

using Core::HID::ControllerTriggerType;
using Service::HID::NpadInputPublish;

TEST_CASE("Only changes the game reads from the npad LIFOs publish at once", "[hid][npad]") {
    CHECK(NpadInputPublish::Publishes(ControllerTriggerType::Button));
    CHECK(NpadInputPublish::Publishes(ControllerTriggerType::Stick));
    CHECK(NpadInputPublish::Publishes(ControllerTriggerType::Trigger));
    CHECK_FALSE(NpadInputPublish::Publishes(ControllerTriggerType::Motion));
    CHECK_FALSE(NpadInputPublish::Publishes(ControllerTriggerType::Battery));
    CHECK_FALSE(NpadInputPublish::Publishes(ControllerTriggerType::Connected));
    CHECK_FALSE(NpadInputPublish::Publishes(ControllerTriggerType::All));
}

TEST_CASE("Input changes before the publish runs share it", "[hid][npad]") {
    NpadInputPublish publish;
    CHECK(publish.Request());
    CHECK_FALSE(publish.Request());
    CHECK_FALSE(publish.Request());

    // Once the publish reads the controllers, a later change needs its own.
    publish.Begin();
    CHECK(publish.Request());
    CHECK_FALSE(publish.Request());
}
