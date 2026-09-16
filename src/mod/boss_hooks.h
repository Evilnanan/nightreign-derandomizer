#pragma once

#include "game_module.h"

#include <cstdint>

namespace derandomizer::mod {

struct NightlordHookSnapshot {
    std::int32_t observed;
    std::int32_t calls;
    bool enabled;
    std::int32_t value;
    void* source_object;
    std::int32_t getter_observed;
    std::int32_t getter_calls;
    std::int32_t getter_corrections;
    std::int32_t getter_last_from;
};

struct EverdarkHookSnapshot {
    std::int32_t observed;
    std::int32_t calls;
    bool enabled;
    std::int32_t value;
    void* source_object;
    std::int32_t corrections;
    std::int32_t last_from;
};

struct BossHookSnapshot {
    NightlordHookSnapshot nightlord;
    EverdarkHookSnapshot everdark;
};

void initialize_boss_hooks(const ModuleInfo& module);
bool nightlord_lock_available();
bool everdark_lock_available();
void verify_boss_hooks();
void set_nightlord_lock(bool enabled, std::int32_t value);
void set_everdark_lock(bool enabled, std::int32_t value);
BossHookSnapshot boss_hook_snapshot();

}  // namespace derandomizer::mod

