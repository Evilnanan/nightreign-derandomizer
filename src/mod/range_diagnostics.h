#pragma once

#include "game_module.h"

namespace derandomizer::mod {

// Development-only instrumentation enabled by `diag_range` in the INI file.
// Installation is attempted at most once because it modifies live game code.
bool install_range_diagnostics(const ModuleInfo& module);
bool range_diagnostics_installed();
void reset_range_diagnostics();
void log_range_diagnostics(const ModuleInfo& module);

}  // namespace derandomizer::mod

