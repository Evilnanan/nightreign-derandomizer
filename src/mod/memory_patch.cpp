#include "memory_patch.h"

#include "logging.h"

#include <climits>
#include <cstring>

namespace derandomizer::mod {
namespace {

constexpr int kMaximumStubRanges = 3;
void* g_stub_ranges[kMaximumStubRanges]{};
std::uint8_t* g_stub_ends[kMaximumStubRanges]{};
const char* g_stub_names[kMaximumStubRanges]{};

const char* stub_owner(void* instruction_pointer) {
    auto* pointer = static_cast<std::uint8_t*>(instruction_pointer);
    for (int i = 0; i < kMaximumStubRanges; ++i) {
        if (g_stub_ranges[i] &&
            pointer >= static_cast<std::uint8_t*>(g_stub_ranges[i]) &&
            pointer < g_stub_ends[i]) {
            return g_stub_names[i];
        }
    }
    return nullptr;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* exception) {
    auto* registers = exception->ContextRecord;
    const char* owner = stub_owner(reinterpret_cast<void*>(registers->Rip));
    logf("!!! EXCEPTION code=0x%08X RIP=%p %s",
         static_cast<unsigned>(exception->ExceptionRecord->ExceptionCode),
         reinterpret_cast<void*>(registers->Rip),
         owner ? "*** INSIDE OUR STUB ***" : "(not in our stubs)");
    logf("    rax=%p rbx=%p rcx=%p rdx=%p",
         reinterpret_cast<void*>(registers->Rax),
         reinterpret_cast<void*>(registers->Rbx),
         reinterpret_cast<void*>(registers->Rcx),
         reinterpret_cast<void*>(registers->Rdx));
    logf("    rbp=%p rsp=%p r12=%p r13=%p",
         reinterpret_cast<void*>(registers->Rbp),
         reinterpret_cast<void*>(registers->Rsp),
         reinterpret_cast<void*>(registers->R12),
         reinterpret_cast<void*>(registers->R13));
    if (owner) logf("    stub owner: %s", owner);
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        logf("    AV %s at %p",
             exception->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" :
             exception->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute",
             reinterpret_cast<void*>(
                 exception->ExceptionRecord->ExceptionInformation[1]));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void* allocate_near(void* target, std::size_t size) {
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const std::uintptr_t granularity = system_info.dwAllocationGranularity
                                         ? system_info.dwAllocationGranularity
                                         : 0x10000;
    const std::uintptr_t target_address = reinterpret_cast<std::uintptr_t>(target);
    std::uintptr_t low = target_address > static_cast<std::uintptr_t>(INT32_MAX)
                           ? target_address - static_cast<std::uintptr_t>(INT32_MAX)
                           : granularity;
    const std::uintptr_t high = target_address + static_cast<std::uintptr_t>(INT32_MAX);
    low = (low + granularity - 1) & ~(granularity - 1);

    constexpr std::uintptr_t probes[] = {
        0x1000000, 0x4000000, 0x10000000, 0x20000000, 0x30000000, 0x40000000,
    };
    for (std::uintptr_t distance : probes) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            std::uintptr_t base = sign > 0 ? target_address + distance
                                           : target_address - distance;
            base &= ~(granularity - 1);
            if (base < low || base > high) continue;
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(base), &region,
                             sizeof(region)) == 0 ||
                region.State != MEM_FREE) {
                continue;
            }
            void* allocation = VirtualAlloc(reinterpret_cast<LPVOID>(base), size,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
            if (allocation) return allocation;
        }
    }

    std::uintptr_t address = low;
    int steps = 0;
    while (address < high && steps < 4096) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region,
                         sizeof(region)) == 0) {
            break;
        }
        if (region.State == MEM_FREE && region.RegionSize >= size) {
            void* allocation = VirtualAlloc(reinterpret_cast<LPVOID>(address), size,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
            if (allocation) return allocation;
        }
        const std::uintptr_t next =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = (next + granularity - 1) & ~(granularity - 1);
        ++steps;
    }
    return nullptr;
}

}  // namespace

void register_stub_range(void* base, std::size_t length, const char* name) {
    for (int i = 0; i < kMaximumStubRanges; ++i) {
        if (!g_stub_ranges[i]) {
            g_stub_ranges[i] = base;
            g_stub_ends[i] = static_cast<std::uint8_t*>(base) + length;
            g_stub_names[i] = name;
            diagf("stub range registered: %s [%p, %p)", name, base,
                  static_cast<void*>(g_stub_ends[i]));
            return;
        }
    }
}

void install_crash_filter() {
    SetUnhandledExceptionFilter(crash_filter);
}

std::uint8_t* allocate_stub_memory(void* site, std::size_t size,
                                   const char* description) {
    auto* allocation = static_cast<std::uint8_t*>(allocate_near(site, size));
    if (!allocation) {
        diagf("%s: could not allocate stub memory within 2GB of %p",
              description, site);
        return nullptr;
    }
    const std::int64_t delta = reinterpret_cast<std::int64_t>(allocation) -
                               reinterpret_cast<std::int64_t>(site);
    diagf("%s: stub at %p (%+lld bytes from the hook site)", description,
          allocation, static_cast<long long>(delta));
    if (delta > INT32_MAX || delta < INT32_MIN) {
        diagf("%s: stub still out of rel32 range - refusing", description);
        VirtualFree(allocation, 0, MEM_RELEASE);
        return nullptr;
    }
    return allocation;
}

bool patch_bytes(void* destination, const void* source, std::size_t length) {
#if defined(DERANDOMIZER_EMIT_ONLY)
    (void)destination;
    (void)source;
    (void)length;
    return true;
#else
    DWORD old_protection = 0;
    if (!VirtualProtect(destination, length, PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        diagf("VirtualProtect(%p,%zu) failed: %lu", destination, length,
              GetLastError());
        return false;
    }
    std::memcpy(destination, source, length);
    VirtualProtect(destination, length, old_protection, &old_protection);
    FlushInstructionCache(GetCurrentProcess(), destination, length);
    return true;
#endif
}

}  // namespace derandomizer::mod

