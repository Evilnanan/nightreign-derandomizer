#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace derandomizer::mod {

void register_stub_range(void* base, std::size_t length, const char* name);
void install_crash_filter();
std::uint8_t* allocate_stub_memory(void* site, std::size_t size,
                                   const char* description);
bool patch_bytes(void* destination, const void* source, std::size_t length);

}  // namespace derandomizer::mod

