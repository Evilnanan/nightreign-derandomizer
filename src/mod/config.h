#pragma once

#include <cstdint>
#include <windows.h>

namespace derandomizer::mod {

struct Config {
    std::int64_t seed_setting = -1;
    std::int32_t nightlord = -1;
    std::int32_t everdark = -1;
    bool log = false;
    bool diag_range = false;
};

void initialize_config(HMODULE self);
void reload_config();
const Config& config();
const wchar_t* config_path();

bool nightlord_id_valid(std::int32_t id);
bool seed_value_valid(std::int64_t seed);
bool everdark_value_valid(std::int32_t value);
bool nightlord_has_no_everdark(std::int32_t nightlord);
std::int32_t effective_everdark_value(std::int32_t nightlord,
                                      std::int32_t configured);
void log_config_summary(const char* event);

}  // namespace derandomizer::mod
