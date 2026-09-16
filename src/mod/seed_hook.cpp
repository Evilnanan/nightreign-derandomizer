#include "seed_hook.h"

#include "logging.h"
#include "memory_patch.h"

#include <windows.h>

#include <climits>
#include <cstdio>
#include <cstring>

namespace derandomizer::mod {
namespace {

constexpr std::uint32_t kSeedOffset = 0xB54;
constexpr int kSeedHistoryCapacity = 16;

std::uint8_t* g_seed_site = nullptr;
std::uint8_t* g_seed_stub = nullptr;
std::uint8_t g_original_seed_bytes[6]{};
bool g_seed_installed = false;

volatile LONG g_seed_target = 0;
volatile LONG g_corrector_enabled = 0;
volatile LONG g_seed_corrections = 0;
volatile LONG g_seed_observations = 0;
volatile LONG g_seed_checks = 0;
volatile std::uint8_t* g_seed_state = nullptr;

volatile LONG g_history_size = 0;
volatile LONG g_history_values[kSeedHistoryCapacity]{};
volatile LONG g_history_times[kSeedHistoryCapacity]{};
volatile LONG g_history_fixed[kSeedHistoryCapacity]{};

std::size_t emit_seed_local_stub(std::uint8_t* code, std::size_t capacity,
                                 std::uint64_t stub_address,
                                 std::uint64_t return_address,
                                 std::uint32_t seed,
                                 std::int8_t frame_displacement) {
    std::size_t length = 0;
    if (capacity < 32) return 0;
    code[length++] = 0xC7;
    code[length++] = 0x45;
    code[length++] = static_cast<std::uint8_t>(frame_displacement);
    std::memcpy(code + length, &seed, 4);
    length += 4;
    code[length++] = 0xC7;
    code[length++] = 0x45;
    code[length++] = static_cast<std::uint8_t>(frame_displacement + 4);
    code[length++] = 0;
    code[length++] = 0;
    code[length++] = 0;
    code[length++] = 0;
    code[length++] = 0xE9;
    const std::size_t jump_displacement = length;
    length += 4;
    const std::int64_t delta = static_cast<std::int64_t>(return_address) -
        (static_cast<std::int64_t>(stub_address) +
         static_cast<std::int64_t>(jump_displacement) + 4);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    *reinterpret_cast<std::int32_t*>(code + jump_displacement) =
        static_cast<std::int32_t>(delta);
    return length;
}

std::size_t emit_seed_site_patch(std::uint8_t* output,
                                 std::uint64_t site_address,
                                 std::uint64_t stub_address) {
    const std::int64_t delta = static_cast<std::int64_t>(stub_address) -
                               (static_cast<std::int64_t>(site_address) + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    output[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(output + 1) = static_cast<std::int32_t>(delta);
    output[5] = 0x90;
    return 6;
}

DWORD WINAPI seed_corrector_thread(LPVOID) {
    std::uint32_t start = GetTickCount();
    LONG was_enabled = 0;

    for (;;) {
        const LONG enabled = InterlockedCompareExchange(&g_corrector_enabled, 0, 0);
        if (!enabled) {
            if (was_enabled) start = GetTickCount();
            was_enabled = 0;
            Sleep(20);
            continue;
        }
        if (!was_enabled) start = GetTickCount();
        was_enabled = 1;

        const LONG checks = InterlockedIncrement(&g_seed_checks);
        auto* state = const_cast<std::uint8_t*>(g_seed_state);
        const LONG target = InterlockedCompareExchange(&g_seed_target, 0, 0);
        if (state) {
            const std::int32_t current =
                *reinterpret_cast<volatile std::int32_t*>(state + kSeedOffset);
            if (current != static_cast<std::int32_t>(target) && current > 0) {
                const LONG index = InterlockedIncrement(&g_history_size) - 1;
                if (index < kSeedHistoryCapacity) {
                    InterlockedExchange(&g_history_values[index], current);
                    InterlockedExchange(&g_history_times[index],
                                        static_cast<LONG>(GetTickCount() - start));
                }
                InterlockedIncrement(&g_seed_observations);
                *reinterpret_cast<volatile std::int32_t*>(state + kSeedOffset) =
                    static_cast<std::int32_t>(target);
                InterlockedIncrement(&g_seed_corrections);
                if (index < kSeedHistoryCapacity)
                    InterlockedExchange(&g_history_fixed[index], 1);
            }
        }
        if ((checks & 0xFFFF) == 0) SwitchToThread();
        Sleep(1);
    }
}

}  // namespace

bool install_seed_stub(std::uint8_t* site, std::int8_t frame_displacement) {
    if (g_seed_installed) return true;
    if (!site) return false;
    if (frame_displacement >= 0) {
        diagf("seed stub: implausible frame displacement %d at %p",
              static_cast<int>(frame_displacement), site);
        return false;
    }

    std::uint8_t* stub = allocate_stub_memory(site, 64, "seed local stub");
    if (!stub) return false;
    const std::size_t stub_length = emit_seed_local_stub(
        stub, 64, reinterpret_cast<std::uint64_t>(stub),
        reinterpret_cast<std::uint64_t>(site + 6), 0, frame_displacement);
    if (!stub_length) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, stub_length);
    register_stub_range(stub, stub_length, "seed-local");

    std::memcpy(g_original_seed_bytes, site, 6);
    std::uint8_t patch[6]{};
    if (emit_seed_site_patch(patch, reinterpret_cast<std::uint64_t>(site),
                             reinterpret_cast<std::uint64_t>(stub)) != 6 ||
        !patch_bytes(site, patch, 6)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        diagf("seed stub: cave out of rel32 range from %p (or patch failed)", site);
        return false;
    }

    g_seed_site = site;
    g_seed_stub = stub;
    g_seed_installed = true;
    diagf("seed stub installed at %p -> cave %p (stole %02X %02X %02X %02X %02X %02X, "
          "writes [rbp%+d], jmp rel32=%d)",
          site, stub, g_original_seed_bytes[0], g_original_seed_bytes[1],
          g_original_seed_bytes[2], g_original_seed_bytes[3],
          g_original_seed_bytes[4], g_original_seed_bytes[5],
          static_cast<int>(frame_displacement),
          *reinterpret_cast<std::int32_t*>(patch + 1));
    return true;
}

bool apply_seed_stub(std::uint32_t seed) {
    return g_seed_installed && patch_bytes(g_seed_stub + 3, &seed, sizeof(seed));
}

void disable_seed_stub() {
    if (!g_seed_installed) return;
    if (patch_bytes(g_seed_site, g_original_seed_bytes, sizeof(g_original_seed_bytes))) {
        diagf("seed stub removed from %p", g_seed_site);
        g_seed_installed = false;
    }
}

bool seed_stub_installed() {
    return g_seed_installed;
}

void start_seed_corrector() {
    HANDLE thread = CreateThread(nullptr, 0, seed_corrector_thread, nullptr, 0, nullptr);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(thread);
        diagf("seed fast thread: started (dedicated field corrector)");
    } else {
        diagf("seed fast thread: CreateThread failed (%lu) - poll only", GetLastError());
    }
}

void update_seed_corrector(std::uint8_t* state, std::uint32_t target) {
    g_seed_state = state;
    InterlockedExchange(&g_seed_target, static_cast<LONG>(target));
    if (InterlockedCompareExchange(&g_corrector_enabled, 0, 0)) return;

    InterlockedExchange(&g_history_size, 0);
    InterlockedExchange(&g_seed_observations, 0);
    InterlockedExchange(&g_seed_corrections, 0);
    InterlockedExchange(&g_seed_checks, 0);
    for (int i = 0; i < kSeedHistoryCapacity; ++i) {
        InterlockedExchange(&g_history_values[i], 0);
        InterlockedExchange(&g_history_times[i], 0);
        InterlockedExchange(&g_history_fixed[i], 0);
    }
    InterlockedExchange(&g_corrector_enabled, 1);
}

void disable_seed_corrector() {
    InterlockedExchange(&g_corrector_enabled, 0);
}

SeedDiagnostics seed_diagnostics() {
    return {
        InterlockedCompareExchange(&g_seed_corrections, 0, 0),
        InterlockedCompareExchange(&g_seed_observations, 0, 0),
        InterlockedCompareExchange(&g_seed_checks, 0, 0),
    };
}

int format_seed_history(char* output, std::size_t output_length) {
    int entries = 0;
    std::size_t used = 0;
    LONG total = InterlockedCompareExchange(&g_history_size, 0, 0);
    if (total > kSeedHistoryCapacity) total = kSeedHistoryCapacity;
    if (output_length) output[0] = 0;
    for (LONG i = 0; i < total; ++i) {
        const LONG value = InterlockedCompareExchange(&g_history_values[i], 0, 0);
        const LONG time = InterlockedCompareExchange(&g_history_times[i], 0, 0);
        const LONG fixed = InterlockedCompareExchange(&g_history_fixed[i], 0, 0);
        const int written = _snprintf_s(
            output + used, output_length - used, _TRUNCATE,
            "%s0x%08X@+%ldms%s", i ? ", " : "",
            static_cast<std::uint32_t>(value), time, fixed ? "->fixed" : "->LOST");
        if (written < 0) break;
        used += static_cast<std::size_t>(written);
        ++entries;
    }
    return entries;
}

}  // namespace derandomizer::mod

