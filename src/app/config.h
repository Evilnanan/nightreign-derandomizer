#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace derandomizer {

// Produces the complete derandomizer.ini payload. The caller is responsible
// for choosing the destination and preserving any existing file.
std::string makeModConfig(std::uint32_t seed, int nightlord, int everdark);

// Atomically replaces the destination with a generated derandomizer.ini.
// The parent directory must already exist.
bool writeModConfig(const std::filesystem::path& destination,
                    std::uint32_t seed,
                    int nightlord,
                    int everdark,
                    std::wstring& error);

}  // namespace derandomizer
