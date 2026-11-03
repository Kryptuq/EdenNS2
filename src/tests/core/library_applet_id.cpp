// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/am/frontend/applet_software_keyboard_types.h"
#include "core/hle/service/am/service/library_applet_creator.h"

using Service::AM::AppletId;

TEST_CASE("Switch 2 software keyboard applet id maps to the software keyboard", "[core][applet]") {
    CHECK(Service::AM::NormalizeLibraryAppletId(0x1020011) == AppletId::SoftwareKeyboard);
    CHECK(Service::AM::NormalizeLibraryAppletId(0x11) == AppletId::SoftwareKeyboard);
    CHECK(Service::AM::NormalizeLibraryAppletId(0x10) == AppletId::ProfileSelect);
    // Unknown ids with flags stay unknown (stub applet).
    CHECK(static_cast<u32>(Service::AM::NormalizeLibraryAppletId(0x1020010)) == 0x1020010);
}

TEST_CASE("Switch 2 keyboard configuration fields sit where a real configuration has them", "[core][applet]") {
    using namespace Service::AM::Frontend;
    // From the Yoshi demo's name entry: type 4 at 0, prediction at 0x1a, cursor 1 at 0x20, header
    // text at 0x24, maximum length 10 at 0x3ac.
    CHECK(offsetof(SwkbdConfigCommon, use_prediction) == 0x1a);
    CHECK(offsetof(SwkbdConfigCommon, initial_cursor_position) == 0x20);
    CHECK(offsetof(SwkbdConfigCommon, header_text) == 0x24);
    CHECK(offsetof(SwkbdConfigCommon, max_text_length) == 0x3ac);
    CHECK(SwkbdConfigSwitch2Size >= sizeof(SwkbdConfigCommon) + sizeof(SwkbdConfigNew));
}
