#include "range_diagnostics.h"

#include "logging.h"
#include "memory_patch.h"

#include <windows.h>

#include <climits>
#include <cstdint>
#include <cstring>

namespace derandomizer::mod {
namespace {

struct InlineHook {
    std::uint8_t* target = nullptr;
    std::size_t steal = 0;
    std::uint8_t orig[32]{};
    void* stub = nullptr;
    bool installed = false;
};

bool g_install_attempted = false;

// ---------------------------------------------------------------------------
// Range() argument recorder (development only, default off)
//
// Why this exists. The published seed->pattern model says the pattern is
// `set.patternIds[v % count]` where the set wins a weighted lottery over a
// per-Nightlord row list and `v = SFMT19937(seed).next_uint32()`. Every part of
// that has been checked in isolation - the SFMT port reproduces the exe's own
// gen_rand_all bit for bit, Range() is literally `min + next() % n`, the exported
// LotResultMapPatternFlag rows are already in ascending patternId order - yet the
// model contradicts the mod's own logged rolls (seed=0 gives 12/52/172/292/1107
// where it predicts 16/56/176/296/1096). Worse, for the seed=0 / Gladius roll no
// index modulus exists that turns the verified draw into the observed pattern id,
// so the candidate vector cannot be a contiguous ascending id range at all.
//
// The two numbers that are still unknown are exactly the two Range() arguments:
//   * the lottery call passes max = totalWeight - 1,
//   * the pattern-index call passes max = candidateCount - 1.
// Recording them settles both without guessing a single stack offset, which is
// what sank the static reading of 0x6B2AA0 (its frame mapping is not reliable).
//
// The stub only writes into a global struct; it never calls into our code from
// the game thread, so it cannot deadlock or allocate. The worker formats it.
// ---------------------------------------------------------------------------
static const int RANGE_DIAG_MAX = 128;

struct RangeDiag {
    volatile LONG     total;                // every Range() call (liveness proof)
    volatile LONG     n;                    // calls recorded since the last reset
    volatile uint32_t ret[RANGE_DIAG_MAX];  // ring: call site (return address, low 32)
    volatile uint32_t mn [RANGE_DIAG_MAX];  // ring: Range min
    volatile uint32_t mx [RANGE_DIAG_MAX];  // ring: Range max (count-1 for the index draw)
    volatile uint32_t val[RANGE_DIAG_MAX];  // ring: the raw next() value before `% n`
    volatile uint32_t obj[RANGE_DIAG_MAX];  // ring: the RNG object (`this`) of the call
};

static RangeDiag g_range_diag = {};
static InlineHook g_range_hook;

// Range() prologue, read from the 1.3.3.0 image:
//   48 89 5C 24 08   mov [rsp+8],rbx   <- stolen (5 bytes)
//   57               push rdi
//   48 83 EC 20      sub rsp,0x20
//   41 8B D8         mov ebx,r8d       ; max
//   8B FA            mov edi,edx       ; min
//   2B DA / 83 C3 01 sub ebx,edx / add ebx,1   ; n = max-min+1
//   74               je  (n == 0 -> return min)
static const uint8_t SIG_RANGE[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
    0x41, 0x8B, 0xD8, 0x8B, 0xFA, 0x2B, 0xDA, 0x83, 0xC3, 0x01, 0x74
};
static const char MSK_RANGE[] = "xxxxxxxxxxxxxxxxxxxxx";

// Emit the recorder into `code`. Pure: no allocation, no patching, so the test
// harness can obtain the exact shipped bytes and disassemble them. `rel_back` is
// the address the final jmp must reach; it is written relative to `stub_va`, the
// address the code will be executed from.
static size_t emit_range_diag_stub(uint8_t* code, size_t cap, uint64_t stub_va,
                                   uint64_t rel_back)
{
    size_t n = 0;
    if (cap < 128) return 0;                            // fixed 88-byte shape

    code[n++] = 0x49; code[n++] = 0xBA;                 // mov r10, imm64
    uint64_t diag = (uint64_t)&g_range_diag;
    memcpy(code + n, &diag, 8); n += 8;

    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x00;                 // lock inc [r10+0]  (total)

    code[n++] = 0x41; code[n++] = 0x8B; code[n++] = 0x42;
    code[n++] = 0x04;                                   // mov eax,[r10+4]   (n)

    // Ring index: `n & (MAX-1)`. A plain "stop at MAX" bound would keep the
    // FIRST MAX calls of the window and throw away the last ones - and the calls
    // that matter are the last ones before the pattern is stored. Wrapping keeps
    // the most recent MAX calls instead, which is what the dump reads.
    code[n++] = 0x83; code[n++] = 0xE0; code[n++] = (uint8_t)(RANGE_DIAG_MAX - 1);
    code[n++] = 0xC1; code[n++] = 0xE0; code[n++] = 0x02;   // and eax,MAX-1 / shl 2

    code[n++] = 0x44; code[n++] = 0x8B; code[n++] = 0x0C;
    code[n++] = 0x24;                                   // mov r9d,[rsp]  (retaddr)

    code[n++] = 0x49; code[n++] = 0xBB;                 // mov r11, imm64
    uint64_t ret_a = (uint64_t)&g_range_diag.ret[0];
    memcpy(code + n, &ret_a, 8); n += 8;
    code[n++] = 0x45; code[n++] = 0x89; code[n++] = 0x0C;
    code[n++] = 0x03;                                   // mov [r11+rax],r9d

    code[n++] = 0x49; code[n++] = 0xBB;
    uint64_t mn_a = (uint64_t)&g_range_diag.mn[0];
    memcpy(code + n, &mn_a, 8); n += 8;
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x14;
    code[n++] = 0x03;                                   // mov [r11+rax],edx (min)

    code[n++] = 0x49; code[n++] = 0xBB;
    uint64_t mx_a = (uint64_t)&g_range_diag.mx[0];
    memcpy(code + n, &mx_a, 8); n += 8;
    code[n++] = 0x45; code[n++] = 0x89; code[n++] = 0x04;
    code[n++] = 0x03;                                   // mov [r11+rax],r8d (max)

    code[n++] = 0x49; code[n++] = 0xBB;
    uint64_t obj_a = (uint64_t)&g_range_diag.obj[0];
    memcpy(code + n, &obj_a, 8); n += 8;
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x0C;
    code[n++] = 0x03;                                   // mov [r11+rax],ecx (this)

    code[n++] = 0xF0; code[n++] = 0x41; code[n++] = 0xFF;
    code[n++] = 0x42; code[n++] = 0x04;                 // lock inc [r10+4]  (n)

    code[n++] = 0x48; code[n++] = 0x89; code[n++] = 0x5C;
    code[n++] = 0x24; code[n++] = 0x08;                 // stolen: mov [rsp+8],rbx

    code[n++] = 0xE9;                                   // jmp back
    size_t j_rel = n;
    n += 4;
    int64_t delta = (int64_t)rel_back - ((int64_t)stub_va + (int64_t)j_rel + 4);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    *(int32_t*)(code + j_rel) = (int32_t)delta;
    return n;
}

static void* build_range_diag_stub(uint8_t* site, void* back)
{
    uint8_t* p = allocate_stub_memory(site, 160, "range diag stub");
    if (!p) return nullptr;
    size_t n = emit_range_diag_stub(p, 160, (uint64_t)p, (uint64_t)back);
    if (!n) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), p, n);
    diagf("range diag stub: %zu bytes", n);
    register_stub_range(p, n, "range-diag");
    return p;
}

static bool range_diag_hook_install(uint8_t* site, void* stub)
{
    if (!site || !stub) return false;
    if (memcmp(site, SIG_RANGE, sizeof(SIG_RANGE)) != 0) return false;
    g_range_hook.target = site;
    g_range_hook.stub = stub;
    // The patch must be EXACTLY as long as the bytes the stub re-emits. Writing
    // the usual `E9 rel32` + 3 NOPs here (as the getter hooks do) destroys
    // `push rdi` at site+5 and the first two bytes of `sub rsp,0x20` at site+6,
    // so the jump back to site+5 lands on `90 90 90 EC 20` - and `EC` is
    // `in al,dx`, a privileged instruction. That crashed the game on the first
    // in-game attempt at 21:22 and is why this is asserted rather than assumed.
    const size_t steal = 5;
    static_assert(sizeof(SIG_RANGE) >= 5, "signature must cover the stolen bytes");
    g_range_hook.steal = steal;
    memcpy(g_range_hook.orig, site, steal);
    int64_t delta = (int64_t)stub - ((int64_t)site + (int64_t)steal);
    if (delta > INT32_MAX || delta < INT32_MIN) return false;
    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    *(int32_t*)(patch + 1) = (int32_t)delta;
    if (!patch_bytes(site, patch, steal)) return false;
    g_range_hook.installed = true;
    diagf("range diag hook: installed at %p -> stub %p (%zu bytes stolen)",
          site, stub, steal);
    // Record what the resume point looks like, so if this ever crashes the log
    // already shows whether the surrounding instructions survived the patch.
    diagf("range diag hook: stolen %02X %02X %02X %02X %02X, resume at %p = "
          "%02X %02X %02X %02X %02X %02X %02X %02X",
          g_range_hook.orig[0], g_range_hook.orig[1], g_range_hook.orig[2],
          g_range_hook.orig[3], g_range_hook.orig[4],
          site + steal, site[5], site[6], site[7], site[8],
          site[9], site[10], site[11], site[12]);
    return true;
}

static void range_diag_reset()
{
    if (!g_range_hook.installed) return;
    InterlockedExchange((volatile LONG*)&g_range_diag.n, 0);
}

// ---------------------------------------------------------------------------
// The draw recorder: what `next()` actually returned.
//
// The argument recorder proved the model's TABLES are right - the weighted
// lottery really is Range(0,15599) (total 15600) and the index draw really is
// Range(0,19) (20 candidates), exactly what the prediction model computes. Yet
// seed 0 yields pattern 12 where the model says 16, i.e. the index draw returned
// 12 where `SFMT19937(0).next_uint32() % 20` is 16. So the disagreement is in the
// VALUE the generator hands to Range, and the only way to settle it is to read
// that value instead of re-deriving it.
//
// Hook point: inside Range, on the instruction that consumes the draw -
//
//     0x140F563DC  33 D2        xor edx,edx
//     0x140F563DE  F7 F3        div ebx
//     0x140F563E0  8D 04 17     lea eax,[rdi+rdx]
//
// eax on entry to that sequence IS the raw next() value, so recording it here
// needs no stack assumptions at all. The three instructions (7 bytes) are stolen
// whole - a 5-byte jmp may not split an instruction, which is the mistake that
// crashed the first revision of the entry hook - and re-emitted in the stub.
// ---------------------------------------------------------------------------
static const uint8_t SIG_RANGE_DRAW[] = {
    0x33, 0xD2, 0xF7, 0xF3, 0x8D, 0x04, 0x17, 0x48, 0x8B, 0x5C, 0x24, 0x30
};
static const char MSK_RANGE_DRAW[] = "xxxxxxxxxxxx";
static const size_t RANGE_DRAW_OFF = 0x1C;   // from Range's entry to the div
static const size_t RANGE_DRAW_STEAL = 7;    // xor edx,edx / div ebx / lea eax,[rdi+rdx]
static InlineHook g_range_draw_hook;

static size_t emit_range_draw_stub(uint8_t* code, size_t cap, uint64_t stub_va,
                                   uint64_t rel_back)
{
    size_t n = 0;
    if (cap < 64) return 0;

    code[n++] = 0x49; code[n++] = 0xBA;                 // mov r10, imm64
    uint64_t diag = (uint64_t)&g_range_diag;
    memcpy(code + n, &diag, 8); n += 8;

    code[n++] = 0x41; code[n++] = 0x8B; code[n++] = 0x4A;
    code[n++] = 0x04;                                   // mov ecx,[r10+4]  (n)
    code[n++] = 0x83; code[n++] = 0xE9; code[n++] = 0x01;   // sub ecx,1  (this call)
    code[n++] = 0x83; code[n++] = 0xE1; code[n++] = (uint8_t)(RANGE_DIAG_MAX - 1);

    code[n++] = 0x49; code[n++] = 0xBA;                 // mov r10, imm64
    uint64_t val_a = (uint64_t)&g_range_diag.val[0];
    memcpy(code + n, &val_a, 8); n += 8;
    code[n++] = 0x41; code[n++] = 0x89; code[n++] = 0x04;
    code[n++] = 0x8A;                                   // mov [r10+rcx*4],eax

    code[n++] = 0x33; code[n++] = 0xD2;                 // xor edx,edx   (stolen)
    code[n++] = 0xF7; code[n++] = 0xF3;                 // div ebx       (stolen)
    code[n++] = 0x8D; code[n++] = 0x04; code[n++] = 0x17;   // lea eax,[rdi+rdx]

    code[n++] = 0xE9;                                   // jmp back
    size_t j_rel = n;
    n += 4;
    int64_t delta = (int64_t)rel_back - ((int64_t)stub_va + (int64_t)j_rel + 4);
    if (delta > INT32_MAX || delta < INT32_MIN) return 0;
    *(int32_t*)(code + j_rel) = (int32_t)delta;
    return n;
}

static bool range_draw_hook_install(uint8_t* site, void* stub)
{
    if (!site || !stub) return false;
    if (memcmp(site, SIG_RANGE_DRAW, sizeof(SIG_RANGE_DRAW)) != 0) return false;
    g_range_draw_hook.target = site;
    g_range_draw_hook.stub = stub;
    g_range_draw_hook.steal = RANGE_DRAW_STEAL;
    memcpy(g_range_draw_hook.orig, site, RANGE_DRAW_STEAL);
    int64_t delta = (int64_t)stub - ((int64_t)site + 5);
    if (delta > INT32_MAX || delta < INT32_MIN) return false;
    // Exactly 5 bytes: bytes site+5 and site+6 are the tail of the stolen `lea`
    // and are never executed again (the stub re-emits the whole instruction and
    // returns to site+7), but they must be left alone rather than NOPed.
    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    *(int32_t*)(patch + 1) = (int32_t)delta;
    if (!patch_bytes(site, patch, 5)) return false;
    g_range_draw_hook.installed = true;
    diagf("range draw hook: installed at %p -> stub %p", site, stub);
    return true;
}

static void* build_range_draw_stub(uint8_t* site, void* back)
{
    uint8_t* p = allocate_stub_memory(site, 96, "range draw stub");
    if (!p) return nullptr;
    size_t n = emit_range_draw_stub(p, 96, (uint64_t)p, (uint64_t)back);
    if (!n) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), p, n);
    diagf("range draw stub: %zu bytes", n);
    register_stub_range(p, n, "range-draw");
    return p;
}

}  // namespace

bool install_range_diagnostics(const ModuleInfo& module) {
    if (g_range_hook.installed || g_install_attempted)
        return g_range_hook.installed;

    g_install_attempted = true;
    int range_hits = 0;
    std::uint8_t* range_site =
        find_unique(module, SIG_RANGE, MSK_RANGE, sizeof(SIG_RANGE), &range_hits);
    if (range_hits != 1 || !range_site) {
        logf("DIAG range hook: anchor not unique (%d hit(s)) - no capture",
             range_hits);
        return false;
    }

    void* stub = build_range_diag_stub(range_site, range_site + 5);
    if (!stub || !range_diag_hook_install(range_site, stub)) {
        logf("DIAG range hook: INSTALL FAILED at +0x%X",
             static_cast<std::uint32_t>(range_site - module.base));
        return false;
    }

    logf("DIAG range hook armed at +0x%X (seed and pattern are logged as usual)",
         static_cast<std::uint32_t>(range_site - module.base));

    std::uint8_t* draw_site = range_site + RANGE_DRAW_OFF;
    void* draw_stub =
        build_range_draw_stub(draw_site, draw_site + RANGE_DRAW_STEAL);
    if (draw_stub && range_draw_hook_install(draw_site, draw_stub)) {
        logf("DIAG range draw hook armed at +0x%X",
             static_cast<std::uint32_t>(draw_site - module.base));
    } else {
        logf("DIAG range draw hook: INSTALL FAILED at +0x%X "
             "(argument capture only)",
             static_cast<std::uint32_t>(draw_site - module.base));
    }
    return true;
}

bool range_diagnostics_installed() {
    return g_range_hook.installed;
}

void reset_range_diagnostics() {
    range_diag_reset();
}

void log_range_diagnostics(const ModuleInfo& module) {
    if (!g_range_hook.installed) return;

    const LONG captured = InterlockedCompareExchange(
        const_cast<volatile LONG*>(&g_range_diag.n), 0, 0);
    const std::uint32_t module_base =
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(module.base));
    logf("DIAG range calls=%ld captured=%ld",
         InterlockedCompareExchange(
             const_cast<volatile LONG*>(&g_range_diag.total), 0, 0),
         captured);

    LONG printed = 0;
    for (LONG i = 0; i < captured && printed < 48; ++i) {
        const LONG ring_index = i & (RANGE_DIAG_MAX - 1);
        const std::uint32_t maximum = g_range_diag.mx[ring_index];
        if (maximum <= 1) continue;
        logf("DIAG   [%ld] ret=+0x%X min=%u max=%u draw=0x%08X "
             "(draw%%n=%u) this=%08X",
             i,
             static_cast<std::uint32_t>(
                 g_range_diag.ret[ring_index] - module_base),
             g_range_diag.mn[ring_index], maximum,
             g_range_diag.val[ring_index],
             maximum ? (g_range_diag.val[ring_index] % (maximum + 1)) : 0,
             g_range_diag.obj[ring_index]);
        ++printed;
    }
    if (printed == 0)
        logf("DIAG   (no Range call with max>1 in the window)");
}

}  // namespace derandomizer::mod

