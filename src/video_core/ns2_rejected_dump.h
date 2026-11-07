// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

namespace VideoCommon {

/// Diagnostic only (NS2, Yoshi): keeps the bytes of each native program the emulator rejects so
/// the lowering can be run on them offline. It never changes what is rejected or how: the caller
/// writes the files and then throws its own error unchanged, and every failure here is swallowed.
///
/// Per rejected program (once per address and code key) it writes, named
/// "<key>-<cs|gfx>-<address>":
///   .native.bin  the captured instructions exactly as passed to LowerToMaxwell (16 bytes each,
///                starting after the program header) - offline lowering input;
///   .mapped.bin  raw memory from the program base (header included) up to the end of the
///                capture plus ExtraBytes, stopping at the first unmapped page - for code past
///                the capture (CALL targets, overruns) and the header;
///   .txt         address, header size, sizes, key, the error text and offset, failing words.
class RejectedProgramDumper {
public:
    using Instruction = Shader::Ampere::Instruction;
    /// Reads `size` bytes at a GPU address; returns false when the range is not mapped.
    using Reader = std::function<bool(u64 address, void* dest, size_t size)>;

    static constexpr size_t PageBytes = 0x1000;
    static constexpr size_t ExtraBytes = 0x10000;
    static constexpr size_t MaxMappedBytes = 0x100000;
    static constexpr size_t MaxPrograms = 256;

    explicit RejectedProgramDumper(std::filesystem::path directory_)
        : directory{std::move(directory_)} {}

    /// Returns the path stem written, or an empty string when nothing was written (already
    /// dumped, limit reached, or an I/O error).
    std::string Dump(u64 program_base, u32 header_size, bool compute,
                     std::span<const Instruction> native, std::string_view reason,
                     u32 error_offset, const Reader& read) noexcept {
        try {
            const u64 key = Shader::Ampere::NativeProgramKey(native);
            {
                std::scoped_lock lock{mutex};
                if (dumped.size() >= MaxPrograms || !dumped.emplace(program_base, key).second) {
                    return {};
                }
            }
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) return {};
            const std::string stem =
                fmt::format("{:016x}-{}-{:x}", key, compute ? "cs" : "gfx", program_base);
            const auto base = directory / stem;
            {
                std::ofstream file(base.string() + ".native.bin", std::ios::binary);
                file.write(reinterpret_cast<const char*>(native.data()),
                           static_cast<std::streamsize>(native.size_bytes()));
                if (!file) return {};
            }
            size_t mapped_size = 0;
            {
                const size_t wanted = std::min<size_t>(
                    header_size + native.size_bytes() + ExtraBytes, MaxMappedBytes);
                std::ofstream file(base.string() + ".mapped.bin", std::ios::binary);
                std::array<u8, PageBytes> block{};
                // Page-aligned chunks so a partially mapped tail is kept up to its last page.
                u64 address = program_base;
                while (mapped_size < wanted) {
                    const size_t to_page_end = PageBytes - static_cast<size_t>(address % PageBytes);
                    const size_t chunk = std::min(to_page_end, wanted - mapped_size);
                    if (!read(address, block.data(), chunk)) break;
                    file.write(reinterpret_cast<const char*>(block.data()),
                               static_cast<std::streamsize>(chunk));
                    mapped_size += chunk;
                    address += chunk;
                }
                if (!file) return {};
            }
            {
                const size_t failed = error_offset / sizeof(Instruction);
                const auto words = failed < native.size() ? native[failed].words
                                                          : std::array<u32, 4>{};
                std::ofstream file(base.string() + ".txt");
                file << fmt::format(
                    "program_base={:#x}\nheader_size={:#x}\nkind={}\nkey={:016x}\n"
                    "native_instructions={}\nnative_bytes={:#x}\nmapped_bytes={:#x}\n"
                    "error_offset={:#x}\nerror_words={:08x} {:08x} {:08x} {:08x}\nreason={}\n",
                    program_base, header_size, compute ? "compute" : "graphics", key,
                    native.size(), native.size_bytes(), mapped_size, error_offset, words[0],
                    words[1], words[2], words[3], reason);
                if (!file) return {};
            }
            return base.string();
        } catch (...) {
            return {};
        }
    }

private:
    std::filesystem::path directory;
    std::mutex mutex;
    std::set<std::pair<u64, u64>> dumped;
};

} // namespace VideoCommon
