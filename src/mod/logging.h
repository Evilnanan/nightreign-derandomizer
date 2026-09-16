#pragma once

#include <windows.h>

namespace derandomizer::mod {

void initialize_logging(HMODULE self);
void set_logging_enabled(bool enabled);

void diagf(const char* format, ...);
void logf(const char* format, ...);

}  // namespace derandomizer::mod

