// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Diagnostic dump of rejected native programs (yoshi-int-build-30): the files must hold exactly
// what the lowering saw, so offline lowering reproduces the rejection.

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "video_core/ns2_rejected_dump.h"

namespace {
using Shader::Ampere::Instruction;

std::vector<char> ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

TEST_CASE("Rejected native program dump is complete and lowers to the same rejection",
          "[video_core][ns2-dump]") {
    const auto directory = std::filesystem::temp_directory_path() / "ns2_rejected_dump_test";
    std::filesystem::remove_all(directory);

    // A mapped region of 3 pages from base 0x10000; the 4th page is unmapped. Header 0x80 bytes,
    // then: NOP, a corrupt IADD3 (control word 0; rejected, from the yoshi-int-build-30 dump), EXIT.
    constexpr u64 base = 0x10000;
    constexpr u32 header = 0x80;
    std::vector<u8> memory(3 * 0x1000);
    for (size_t i = 0; i < memory.size(); ++i) memory[i] = static_cast<u8>(i * 7 + 1);
    const std::array<Instruction, 3> native{{{{0x00007918, 0, 0, 0}},
                                             {{0x00038210, 0x00000005, 0x00000000, 0x00000000}},
                                             {{0x0000794d, 0, 0x03800000, 0}}}};
    std::memcpy(memory.data() + header, native.data(), sizeof(native));
    const auto read = [&](u64 address, void* dest, size_t size) {
        if (address < base || address + size > base + memory.size()) return false;
        std::memcpy(dest, memory.data() + (address - base), size);
        return true;
    };

    const auto lowered = Shader::Ampere::LowerToMaxwell(native);
    REQUIRE_FALSE(static_cast<bool>(lowered));

    VideoCommon::RejectedProgramDumper dumper{directory};
    const auto stem =
        dumper.Dump(base, header, true, native, lowered.error, lowered.error_offset, read);
    REQUIRE_FALSE(stem.empty());

    // .native.bin: byte-identical to the lowering input, and it reproduces the rejection.
    const auto native_file = ReadFile(stem + ".native.bin");
    REQUIRE(native_file.size() == sizeof(native));
    REQUIRE(std::memcmp(native_file.data(), native.data(), sizeof(native)) == 0);
    std::vector<Instruction> reloaded(native_file.size() / sizeof(Instruction));
    std::memcpy(reloaded.data(), native_file.data(), native_file.size());
    const auto relowered = Shader::Ampere::LowerToMaxwell(reloaded);
    REQUIRE_FALSE(static_cast<bool>(relowered));
    REQUIRE(relowered.error == lowered.error);
    REQUIRE(relowered.error_offset == lowered.error_offset);

    // .mapped.bin: header included, stops at the end of the mapped pages (wanted is larger).
    const auto mapped_file = ReadFile(stem + ".mapped.bin");
    REQUIRE(mapped_file.size() == memory.size());
    REQUIRE(std::memcmp(mapped_file.data(), memory.data(), memory.size()) == 0);

    // .txt names the reason and offset.
    const auto text_file = ReadFile(stem + ".txt");
    const std::string text(text_file.begin(), text_file.end());
    REQUIRE(text.find("reason=" + std::string{lowered.error}) != std::string::npos);
    REQUIRE(text.find("header_size=0x80") != std::string::npos);
    REQUIRE(text.find("kind=compute") != std::string::npos);

    // The same program is written once.
    REQUIRE(dumper.Dump(base, header, true, native, lowered.error, lowered.error_offset, read)
                .empty());
    std::filesystem::remove_all(directory);
}

} // namespace
