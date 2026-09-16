#include "boss_hooks.h"

#include "logging.h"
#include "memory_patch.h"

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstring>

namespace derandomizer::mod {
namespace {

constexpr std::uint32_t OFF_NL_SOURCE = 0x17C;
constexpr std::uint32_t OFF_EVERDARK = 0x17D;

// Nightlord source-field/map-key synchronisation point.
//
// Both callers of the sortie/map initialiser read the actual Nightlord from
// byte [rdi+0x17C] through vtable slot +0x188, then put that value in r9b before
// `call 0x6AEC40`. The callee uses r9b as a map-pattern filter key; changing only
// r9 therefore changes the candidate map block but not the selected Nightlord.
//
// The source getter itself is also hooked. That earlier hook matters: the game
// renders/caches the Nightlord name before either SetupMapPatternInfo CALL, so a
// wrapper at those CALLs can make the eventual map consistent while leaving the
// expedition UI on the game's original roll. The getter hook is the authoritative
// lock; this later CALL wrapper remains as a second consistency layer and a
// diagnostic boundary for the map key.
struct NightlordHookState {
    volatile LONG observed;       // game-supplied r9d
    volatile LONG calls;          // proves the hook executed even if id repeats
    volatile LONG enabled;        // zero = observation-only pass-through
    volatile LONG value;          // forced r9d when enabled
    volatile LONG64 source_obj;   // getter rcx / late caller rdi: Nightlord source object
    volatile LONG getter_observed;    // raw source byte before the getter forces it
    volatile LONG getter_calls;       // every source getter invocation
    volatile LONG getter_corrections; // raw source byte differed and was replaced
    volatile LONG getter_last_from;   // last differing game-selected value
};
static NightlordHookState g_nl_hook_state = { -1, 0, 0, -1, 0, -1, 0, 0, -1 };
static void*    g_nl_stub = nullptr;
static uint8_t* g_nl_sites[2] = {};
static bool     g_nl_hooks_ready = false;

// The independent normal/Everdark byte is consumed through the next vtable
// slot. Keep its state separate so the two locks can be enabled and observed
// independently.
struct EverdarkHookState {
    volatile LONG observed;       // raw byte returned by the game
    volatile LONG calls;
    volatile LONG enabled;
    volatile LONG value;          // 0 normal, 1 Everdark
    volatile LONG64 source_obj;
    volatile LONG corrections;
    volatile LONG last_from;
};
static EverdarkHookState g_evd_hook_state = { -1, 0, 0, -1, 0, 0, -1 };

struct NightlordGetterHook {
    uint8_t* target = nullptr;
    uint8_t  orig[8] = {};
    void*    stub = nullptr;
    bool     installed = false;
};
static NightlordGetterHook g_nl_getter_hook;
static uint8_t**           g_nl_getter_vtable_slot = nullptr;
static volatile LONG       g_nl_getter_repatch = 0;
static NightlordGetterHook g_evd_getter_hook;
static uint8_t**           g_evd_getter_vtable_slot = nullptr;
static volatile LONG       g_evd_getter_repatch = 0;

// Select a one-instruction byte getter through its exact source-object vtable
// slot. The function bodies are not globally unique, so matching only the code
// bytes would risk patching an unrelated class.
static uint8_t* find_source_getter(const ModuleInfo& mod,
                                   uint8_t** candidates, int candidate_count,
                                   uint32_t vtable_offset, const char* label,
                                   uint8_t*** selected_slot_out)
{
    uint8_t* selected = nullptr;
    uint8_t* selected_ref = nullptr;
    int selected_count = 0;
    for (int i = 0; i < candidate_count; ++i) {
        uint64_t address = (uint64_t)candidates[i];
        uint8_t sig[8];
        memcpy(sig, &address, sizeof(sig));
        int refs = 0;
        uint8_t* ref = find_unique(mod, sig, "xxxxxxxx", sizeof(sig), &refs);
        diagf("%s getter candidate %p: %d absolute pointer ref(s)",
             label, candidates[i], refs);
        if (refs != 1) continue;

        // Validate the inferred vtable around the required slot instead of
        // accepting an arbitrary qword that
        // happens to equal the function address.
        if (ref < mod.base + vtable_offset || ((uintptr_t)ref & 7) != 0) continue;
        uint8_t* vtable = ref - vtable_offset;
        void* first = *(void**)vtable;
        void* before = *(void**)(ref - 8);
        void* after = *(void**)(ref + 8);
        auto in_image = [&](void* p) {
            return (uint8_t*)p >= mod.base && (uint8_t*)p < mod.base + mod.size;
        };
        if (!in_image(first) || !in_image(before) || !in_image(after)) continue;

        selected = candidates[i];
        selected_ref = ref;
        ++selected_count;
    }
    if (selected_count != 1) {
        diagf("%s getter: expected one vtable-referenced candidate, got %d",
             label, selected_count);
        return nullptr;
    }
    *selected_slot_out = (uint8_t**)selected_ref;
    diagf("%s getter: selected %p via vtable=%p slot=%p (+0x%X)",
         label, selected, selected_ref - vtable_offset, selected_ref, vtable_offset);
    return selected;
}

// Build the authoritative Nightlord getter hook.
//
// Original getter (8 bytes):
//     movzx eax, byte ptr [rcx+0x17C]
//     ret
//
// The stub reproduces that read exactly in observation mode. With the lock on,
// it replaces a differing source byte before returning the configured value.
// This runs at the first consumer (including the expedition UI), earlier than
// the SetupMapPatternInfo CALL wrapper used by the previous implementation.
static void* build_nightlord_getter_stub(uint8_t* site)
{
    uint8_t code[96];
    size_t n = 0;
    code[n++] = 0x49; code[n++] = 0xBA;                 // mov r10, imm64
    uint64_t state = (uint64_t)&g_nl_hook_state;
    memcpy(code + n, &state, 8); n += 8;
    code[n++] = 0x0F; code[n++] = 0xB6; code[n++] = 0x81;
    memcpy(code + n, &OFF_NL_SOURCE, 4); n += 4;        // movzx eax,byte [rcx+17C]
    code[n++] = 0x49; code[n++] = 0x89; code[n++] = 0x4A;
    code[n++] = 0x10;                                   // mov [r10+16],rcx
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x42;
    code[n++] = 0x18;                                   // mov [r10+24],eax
    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x1C;                 // lock inc [r10+28]
    code[n++] = 0x41; code[n++] = 0x83; code[n++] = 0x7A;
    code[n++] = 0x08; code[n++] = 0x00;                 // cmp [r10+8],0
    code[n++] = 0x74; code[n++] = 0x1D;                 // je pass
    code[n++] = 0x45; code[n++] = 0x8B; code[n++] = 0x5A;
    code[n++] = 0x0C;                                   // mov r11d,[r10+12]
    code[n++] = 0x44; code[n++] = 0x3B; code[n++] = 0xD8; // cmp r11d,eax
    code[n++] = 0x74; code[n++] = 0x10;                 // je return_forced
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x42;
    code[n++] = 0x24;                                   // mov [r10+36],eax
    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x20;                 // lock inc [r10+32]
    code[n++] = 0x44; code[n++] = 0x88; code[n++] = 0x99;
    memcpy(code + n, &OFF_NL_SOURCE, 4); n += 4;        // mov byte [rcx+17C],r11b
    code[n++] = 0x41; code[n++] = 0x8B; code[n++] = 0xC3; // mov eax,r11d
    code[n++] = 0xC3;                                   // ret
    code[n++] = 0xC3;                                   // pass: ret

    uint8_t* p = allocate_stub_memory(site, n, "nightlord getter stub");
    if (!p) return nullptr;
    memcpy(p, code, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    diagf("nightlord getter stub: %zu bytes, early source/read override", n);
    register_stub_range(p, n, "nightlord-getter");
    return p;
}

static bool nightlord_getter_hook_install(uint8_t* site, void* stub)
{
    static const uint8_t expected[8] = {
        0x0F, 0xB6, 0x81, 0x7C, 0x01, 0x00, 0x00, 0xC3
    };
    if (!site || !stub || memcmp(site, expected, sizeof(expected)) != 0) return false;
    g_nl_getter_hook.target = site;
    g_nl_getter_hook.stub = stub;
    memcpy(g_nl_getter_hook.orig, site, sizeof(expected));
    int64_t delta = (int64_t)stub - ((int64_t)site + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return false;
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(int32_t*)(patch + 1) = (int32_t)delta;
    if (!patch_bytes(site, patch, sizeof(patch))) return false;
    g_nl_getter_hook.installed = true;
    diagf("nightlord getter hook: installed at %p -> stub %p", site, stub);
    return true;
}

static int nightlord_getter_hook_verify()
{
    auto& h = g_nl_getter_hook;
    if (!h.installed || !h.target || !h.stub) return 0;
    if (h.target[0] == 0xE9) {
        int32_t disp = 0;
        memcpy(&disp, h.target + 1, 4);
        if (h.target + 5 + disp == (uint8_t*)h.stub) return 0;
    }
    int64_t delta = (int64_t)h.stub - ((int64_t)h.target + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(int32_t*)(patch + 1) = (int32_t)delta;
    return patch_bytes(h.target, patch, sizeof(patch)) ? 1 : 0;
}

// Authoritative normal/Everdark getter. It mirrors the Nightlord getter hook,
// but owns only byte +0x17D and an independent 0/1 configuration value.
static void* build_everdark_getter_stub(uint8_t* site)
{
    uint8_t code[96];
    size_t n = 0;
    code[n++] = 0x49; code[n++] = 0xBA;                 // mov r10, imm64
    uint64_t state = (uint64_t)&g_evd_hook_state;
    memcpy(code + n, &state, 8); n += 8;
    code[n++] = 0x0F; code[n++] = 0xB6; code[n++] = 0x81;
    memcpy(code + n, &OFF_EVERDARK, 4); n += 4;         // movzx eax,byte [rcx+17D]
    code[n++] = 0x49; code[n++] = 0x89; code[n++] = 0x4A;
    code[n++] = 0x10;                                   // mov [r10+16],rcx
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x42;
    code[n++] = 0x00;                                   // mov [r10+0],eax
    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x04;                 // lock inc [r10+4]
    code[n++] = 0x41; code[n++] = 0x83; code[n++] = 0x7A;
    code[n++] = 0x08; code[n++] = 0x00;                 // cmp [r10+8],0
    code[n++] = 0x74; code[n++] = 0x1D;                 // je pass
    code[n++] = 0x45; code[n++] = 0x8B; code[n++] = 0x5A;
    code[n++] = 0x0C;                                   // mov r11d,[r10+12]
    code[n++] = 0x44; code[n++] = 0x3B; code[n++] = 0xD8; // cmp r11d,eax
    code[n++] = 0x74; code[n++] = 0x10;                 // je return_forced
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x42;
    code[n++] = 0x1C;                                   // mov [r10+28],eax
    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x18;                 // lock inc [r10+24]
    code[n++] = 0x44; code[n++] = 0x88; code[n++] = 0x99;
    memcpy(code + n, &OFF_EVERDARK, 4); n += 4;         // mov byte [rcx+17D],r11b
    code[n++] = 0x41; code[n++] = 0x8B; code[n++] = 0xC3; // mov eax,r11d
    code[n++] = 0xC3;                                   // ret
    code[n++] = 0xC3;                                   // pass: ret

    uint8_t* p = allocate_stub_memory(site, n, "Everdark getter stub");
    if (!p) return nullptr;
    memcpy(p, code, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    diagf("Everdark getter stub: %zu bytes, early source/read override", n);
    register_stub_range(p, n, "everdark-getter");
    return p;
}

static bool everdark_getter_hook_install(uint8_t* site, void* stub)
{
    static const uint8_t expected[8] = {
        0x0F, 0xB6, 0x81, 0x7D, 0x01, 0x00, 0x00, 0xC3
    };
    if (!site || !stub || memcmp(site, expected, sizeof(expected)) != 0) return false;
    g_evd_getter_hook.target = site;
    g_evd_getter_hook.stub = stub;
    memcpy(g_evd_getter_hook.orig, site, sizeof(expected));
    int64_t delta = (int64_t)stub - ((int64_t)site + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return false;
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(int32_t*)(patch + 1) = (int32_t)delta;
    if (!patch_bytes(site, patch, sizeof(patch))) return false;
    g_evd_getter_hook.installed = true;
    diagf("Everdark getter hook: installed at %p -> stub %p", site, stub);
    return true;
}

static int everdark_getter_hook_verify()
{
    auto& h = g_evd_getter_hook;
    if (!h.installed || !h.target || !h.stub) return 0;
    if (h.target[0] == 0xE9) {
        int32_t disp = 0;
        memcpy(&disp, h.target + 1, 4);
        if (h.target + 5 + disp == (uint8_t*)h.stub) return 0;
    }
    int64_t delta = (int64_t)h.stub - ((int64_t)h.target + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    uint8_t patch[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(int32_t*)(patch + 1) = (int32_t)delta;
    return patch_bytes(h.target, patch, sizeof(patch)) ? 1 : 0;
}

// Build the shared Nightlord CALL wrapper.
//
//     mov  r10, &g_nl_hook_state
//     mov  [r10+0], r9d              ; observe the game's original value
//     lock inc dword ptr [r10+4]     ; event counter (same id may repeat)
//     mov  [r10+16], rdi             ; source object used by both known callers
//     cmp  dword ptr [r10+8], 0
//     je   pass
//     mov  r9d, [r10+12]             ; forced Nightlord/map key
//     mov  byte ptr [rdi+0x17C], r9b ; update the actual Nightlord source field
// pass:
//     jmp  qword ptr [rip+0]         ; original 0x6AEC40
//
// r10 and flags are volatile in the Windows x64 ABI and the original CALL was
// allowed to destroy both, so the wrapper does not need a stack frame.
static void* build_nightlord_stub(uint8_t* site, void* original)
{
    uint8_t code[64];
    size_t n = 0;
    code[n++] = 0x49; code[n++] = 0xBA;                 // mov r10, imm64
    uint64_t state = (uint64_t)&g_nl_hook_state;
    memcpy(code + n, &state, 8); n += 8;
    code[n++] = 0x45; code[n++] = 0x89; code[n++] = 0x0A; // mov [r10],r9d
    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x04;                 // lock inc [r10+4]
    code[n++] = 0x49; code[n++] = 0x89; code[n++] = 0x7A;
    code[n++] = 0x10;                                   // mov [r10+16],rdi
    code[n++] = 0x41; code[n++] = 0x83; code[n++] = 0x7A;
    code[n++] = 0x08; code[n++] = 0x00;                 // cmp [r10+8],0
    code[n++] = 0x74; code[n++] = 0x0B;                 // je pass (skip 4+7 bytes)
    code[n++] = 0x45; code[n++] = 0x8B; code[n++] = 0x4A;
    code[n++] = 0x0C;                                   // mov r9d,[r10+12]
    code[n++] = 0x44; code[n++] = 0x88; code[n++] = 0x8F;
    memcpy(code + n, &OFF_NL_SOURCE, 4); n += 4;         // mov byte [rdi+0x17C],r9b
    code[n++] = 0xFF; code[n++] = 0x25;                 // jmp [rip+0]
    code[n++] = 0; code[n++] = 0; code[n++] = 0; code[n++] = 0;
    uint64_t target = (uint64_t)original;
    memcpy(code + n, &target, 8); n += 8;

    uint8_t* p = allocate_stub_memory(site, n, "nightlord stub");
    if (!p) return nullptr;
    memcpy(p, code, n);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    diagf("nightlord stub: %zu bytes, observation + conditional source/r9d override", n);
    register_stub_range(p, n, "nightlord-call");
    return p;
}

static bool nightlord_hook_install(uint8_t* site, void* stub)
{
    if (!site || site[0] != 0xE8 || !stub) return false;
    int64_t delta = (int64_t)stub - ((int64_t)site + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return false;
    uint8_t call[5] = { 0xE8, 0, 0, 0, 0 };
    *(int32_t*)(call + 1) = (int32_t)delta;
    return patch_bytes(site, call, sizeof(call));
}

}  // namespace

void initialize_boss_hooks(const ModuleInfo& module) {
    static const std::uint8_t nightlord_call_signature[] = {
        0x45, 0x0F, 0xB6, 0xCE,
        0x44, 0x0F, 0xB6, 0xC6,
        0x0F, 0xB6, 0xD3,
        0xE8, 0, 0, 0, 0
    };
    static const char nightlord_call_mask[] = "xxxxxxxxxxxx????";
    static const std::uint8_t nightlord_getter_signature[] = {
        0x0F, 0xB6, 0x81, 0x7C, 0x01, 0x00, 0x00, 0xC3
    };
    static const char nightlord_getter_mask[] = "xxxxxxxx";
    static const std::uint8_t everdark_getter_signature[] = {
        0x0F, 0xB6, 0x81, 0x7D, 0x01, 0x00, 0x00, 0xC3
    };
    static const char everdark_getter_mask[] = "xxxxxxxx";

    std::uint8_t* nightlord_call_contexts[2]{};
    const int nightlord_call_hits = find_all(
        module, nightlord_call_signature, nightlord_call_mask,
        sizeof(nightlord_call_signature), nightlord_call_contexts, 2);
    diagf("scan: nightlord-call sig %d hit(s)", nightlord_call_hits);

    std::uint8_t* nightlord_getter_candidates[4]{};
    const int nightlord_getter_hits = find_all(
        module, nightlord_getter_signature, nightlord_getter_mask,
        sizeof(nightlord_getter_signature), nightlord_getter_candidates, 4);
    diagf("scan: nightlord-getter body sig %d hit(s)", nightlord_getter_hits);

    std::uint8_t* everdark_getter_candidates[5]{};
    const int everdark_getter_hits = find_all(
        module, everdark_getter_signature, everdark_getter_mask,
        sizeof(everdark_getter_signature), everdark_getter_candidates, 5);
    diagf("scan: Everdark-getter body sig %d hit(s)", everdark_getter_hits);

    std::uint8_t* nightlord_getter =
        nightlord_getter_hits <= 4
            ? find_source_getter(module, nightlord_getter_candidates,
                                 nightlord_getter_hits, 0x188, "nightlord",
                                 &g_nl_getter_vtable_slot)
            : nullptr;
    if (nightlord_getter) {
        void* getter_stub = build_nightlord_getter_stub(nightlord_getter);
        if (!getter_stub ||
            !nightlord_getter_hook_install(nightlord_getter, getter_stub)) {
            diagf("nightlord getter hook: INSTALL FAILED - Nightlord lock disabled");
        }
    } else {
        diagf("nightlord getter hook: anchor unresolved - Nightlord lock disabled");
    }

    std::uint8_t* everdark_getter =
        everdark_getter_hits <= 5
            ? find_source_getter(module, everdark_getter_candidates,
                                 everdark_getter_hits, 0x190, "Everdark",
                                 &g_evd_getter_vtable_slot)
            : nullptr;
    if (everdark_getter) {
        void* getter_stub = build_everdark_getter_stub(everdark_getter);
        if (!getter_stub ||
            !everdark_getter_hook_install(everdark_getter, getter_stub)) {
            diagf("Everdark getter hook: INSTALL FAILED - variant lock disabled");
        }
    } else {
        diagf("Everdark getter hook: anchor unresolved - variant lock disabled");
    }

    if (nightlord_call_hits == 2) {
        void* original = nullptr;
        bool same_target = true;
        for (int i = 0; i < 2; ++i) {
            g_nl_sites[i] = nightlord_call_contexts[i] + 11;
            std::int32_t relative = 0;
            std::memcpy(&relative, g_nl_sites[i] + 1, 4);
            void* target = g_nl_sites[i] + 5 + relative;
            if (!original)
                original = target;
            else if (target != original)
                same_target = false;
            diagf("nightlord call[%d] = %p -> %p", i, g_nl_sites[i], target);
        }
        if (same_target) {
            g_nl_stub = build_nightlord_stub(g_nl_sites[0], original);
            const bool first_installed =
                nightlord_hook_install(g_nl_sites[0], g_nl_stub);
            const bool second_installed =
                nightlord_hook_install(g_nl_sites[1], g_nl_stub);
            g_nl_hooks_ready = first_installed && second_installed;
            diagf("nightlord hooks: %s",
                  g_nl_hooks_ready
                      ? "installed (observation active)"
                      : "INSTALL FAILED");
            if (!g_nl_hooks_ready)
                InterlockedExchange(&g_nl_hook_state.enabled, 0);
        } else {
            diagf("nightlord calls disagree on target - refusing hooks");
        }
    } else {
        diagf("nightlord call anchor expected 2 hits, got %d - observation/lock disabled",
              nightlord_call_hits);
    }
}

bool nightlord_lock_available() {
    return g_nl_hooks_ready && g_nl_getter_hook.installed;
}

bool everdark_lock_available() {
    return g_evd_getter_hook.installed;
}

void verify_boss_hooks() {
    if (g_nl_getter_hook.installed && nightlord_getter_hook_verify()) {
        const LONG repatch = InterlockedIncrement(&g_nl_getter_repatch);
        diagf("NIGHTLORD GETTER: jump drifted - re-applied (repatch #%ld)",
              repatch);
    }
    if (g_evd_getter_hook.installed && everdark_getter_hook_verify()) {
        const LONG repatch = InterlockedIncrement(&g_evd_getter_repatch);
        diagf("EVERDARK GETTER: jump drifted - re-applied (repatch #%ld)",
              repatch);
    }
}

void set_nightlord_lock(bool enabled, std::int32_t value) {
    if (enabled) InterlockedExchange(&g_nl_hook_state.value, value);
    InterlockedExchange(&g_nl_hook_state.enabled, enabled ? 1 : 0);
}

void set_everdark_lock(bool enabled, std::int32_t value) {
    if (enabled) InterlockedExchange(&g_evd_hook_state.value, value);
    InterlockedExchange(&g_evd_hook_state.enabled, enabled ? 1 : 0);
}

BossHookSnapshot boss_hook_snapshot() {
    const auto read = [](volatile LONG* value) {
        return static_cast<std::int32_t>(
            InterlockedCompareExchange(value, 0, 0));
    };
    const auto read_pointer = [](volatile LONG64* value) {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(
            InterlockedCompareExchange64(value, 0, 0)));
    };

    BossHookSnapshot snapshot{};
    snapshot.nightlord.observed = read(&g_nl_hook_state.observed);
    snapshot.nightlord.calls = read(&g_nl_hook_state.calls);
    snapshot.nightlord.enabled = read(&g_nl_hook_state.enabled) != 0;
    snapshot.nightlord.value = read(&g_nl_hook_state.value);
    snapshot.nightlord.source_object = read_pointer(&g_nl_hook_state.source_obj);
    snapshot.nightlord.getter_observed =
        read(&g_nl_hook_state.getter_observed);
    snapshot.nightlord.getter_calls = read(&g_nl_hook_state.getter_calls);
    snapshot.nightlord.getter_corrections =
        read(&g_nl_hook_state.getter_corrections);
    snapshot.nightlord.getter_last_from =
        read(&g_nl_hook_state.getter_last_from);

    snapshot.everdark.observed = read(&g_evd_hook_state.observed);
    snapshot.everdark.calls = read(&g_evd_hook_state.calls);
    snapshot.everdark.enabled = read(&g_evd_hook_state.enabled) != 0;
    snapshot.everdark.value = read(&g_evd_hook_state.value);
    snapshot.everdark.source_object = read_pointer(&g_evd_hook_state.source_obj);
    snapshot.everdark.corrections = read(&g_evd_hook_state.corrections);
    snapshot.everdark.last_from = read(&g_evd_hook_state.last_from);
    return snapshot;
}

}  // namespace derandomizer::mod

