#pragma once

#include <cstddef>
#include <cstdint>

namespace derandomizer::mod {

struct SeedDiagnostics {
    long corrections;
    long observations;
    long checks;
};

bool install_seed_stub(std::uint8_t* site, std::int8_t frame_displacement);
bool apply_seed_stub(std::uint32_t seed);
void disable_seed_stub();
bool seed_stub_installed();

void start_seed_corrector();
void update_seed_corrector(std::uint8_t* state, std::uint32_t target);
void disable_seed_corrector();
SeedDiagnostics seed_diagnostics();
int format_seed_history(char* output, std::size_t output_length);

}  // namespace derandomizer::mod

