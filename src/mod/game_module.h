#pragma once

#include <cstddef>
#include <cstdint>

namespace derandomizer::mod {

struct ModuleInfo {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
};

struct BuildId {
    char file_version[64]{};
    unsigned long time_date_stamp = 0;
    unsigned long size_of_image = 0;
};

bool get_main_module(ModuleInfo& output);
void read_build_id(const ModuleInfo& module, BuildId& output);
bool build_matches(const BuildId& build);
const char* verified_file_version();
void log_build_banner(const BuildId& build);

std::uint8_t* find_unique(const ModuleInfo& module, const std::uint8_t* signature,
                          const char* mask, std::size_t length, int* hits);
int find_all(const ModuleInfo& module, const std::uint8_t* signature,
             const char* mask, std::size_t length, std::uint8_t** output,
             int output_capacity);

}  // namespace derandomizer::mod

