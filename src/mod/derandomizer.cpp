// derandomizer.cpp
// ---------------------------------------------------------------------------
// ELDEN RING NIGHTREIGN - Deep of Night expedition seed / Nightlord locker.
//
// Loaded by me3 as a native DLL. Two jobs:
//
//   1. OBSERVE - read the live expedition state out of nightreign.exe and log
//                the expedition seed + map pattern the game picked, so you can
//                find a layout worth practising.
//   2. LOCK    - force the expedition seed and/or Nightlord. The game remains
//                responsible for resolving the map pattern from that seed, so
//                its spawn point and layout cannot become mismatched.
//
// Anchors are located by byte-signature scan, not by hardcoded RVAs, so the mod
// survives a game patch while the surrounding code shape is unchanged.
//
// Verified against nightreign.exe 1.3.3.0 (app 1.03.3, regulation 1.03.5).
//
// Mechanics (see docs/REVERSE-ENGINEERING.md):
//   nightreign.exe+0x6AEC82  call [rip+x]        draws the expedition seed
//   nightreign.exe+0x6AEC93  mov [rax+0xB54],ebx stores it
//   nightreign.exe+0x6AEEEC  mov [rax+0xB48],r12d stores the resolved pattern
//   nightreign.exe+0xB4BCE6  call 0x6AEC40       passes Nightlord map key in r9b
//   nightreign.exe+0xB4D937  call 0x6AEC40       twin path; rdi owns byte +0x17C
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "boss_hooks.h"
#include "config.h"
#include "game_module.h"
#include "logging.h"
#include "memory_patch.h"
#include "range_diagnostics.h"
#include "seed_hook.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace derandomizer::mod {

// ---------------------------------------------------------------------------
// Expedition state layout (offsets from the object in the global slot)
// ---------------------------------------------------------------------------
static const uint32_t OFF_PATTERN   = 0xB48;  // int32  global map pattern id
static const uint32_t OFF_SEED      = 0xB54;  // uint32 expedition seed
static const uint32_t OFF_NIGHTLORD = 0xB50;  // int32  candidate; unverified
static const uint32_t OFF_NL_ALT    = 0xB4C;  // int32  candidate; unverified
// A different object, passed in rdi by both SetupMapPatternInfo callers. Its
// vtable slot +0x188 is a one-instruction getter for this byte. The constructor
// initialises the containing dword to 0x000000FF, so this MUST remain a byte
// write: +0x17D..+0x17F are separate state.
static const uint32_t OFF_NL_SOURCE = 0x17C;
static BuildId g_build;

// Logging, configuration, module inspection and patching primitives live in
// dedicated translation units. Hook implementations remain below.
// Seed injection and its dedicated field corrector live in seed_hook.cpp.

// Nightlord and Everdark hook implementation lives in boss_hooks.cpp.


// Diagnostics for the direct-write path (no code patching). A non-zero count
// means the inline stub did not cover every store of that field - which is the
// measurement that decides whether the stub is still needed at all.
static volatile LONG g_direct_writes = 0;   // total direct writes to any field
static volatile LONG g_seed_attempts = 0;   // worker iterations with the seed locked

// ---------------------------------------------------------------------------
// Direct write into the state fields.
//
// This is the no-code-patching mechanism. +0xB48 and +0xB54 are written by the
// game with a plain `mov [rax+off], reg`, so they are ordinary writable memory:
// overwriting them from our own thread cannot corrupt anything and needs no stub,
// no code page change and no Arxan interaction.
//
// It exists because the two mechanisms that patch game code each cost crash risk:
// the seed-store hook faulted in-game, and the draw patch was proved not to cover
// every producer of +0xB54.
// ---------------------------------------------------------------------------
static void direct_write_i32(void* base, uint32_t off, int32_t value)
{
    if (!base) return;
    *(volatile int32_t*)((uint8_t*)base + off) = value;
}

// ---------------------------------------------------------------------------
// State access
// ---------------------------------------------------------------------------
static uint8_t* read_state_object(uint8_t* slot)
{
    if (!slot) return nullptr;
    uint8_t* obj = *(uint8_t**)slot;
    if ((uintptr_t)obj < 0x10000 || (uintptr_t)obj > 0x00007FFFFFFFFFFFULL) return nullptr;
    return obj;
}

static bool readable_bytes(const void* ptr, size_t size)
{
    if (!ptr || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    DWORD p = mbi.Protect & 0xFF;
    if (p == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    uintptr_t begin = (uintptr_t)ptr;
    uintptr_t end = begin + size;
    uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return end >= begin && end <= region_end;
}

// Development-only Range() instrumentation lives in range_diagnostics.cpp.

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
// Main worker loop. Map patterns are observed only: forcing one independently
// from its seed can pair a layout with the wrong spawn point.
// ---------------------------------------------------------------------------
static DWORD WINAPI worker(LPVOID param)
{
    const ModuleInfo& mod = *(const ModuleInfo*)param;
    const Config& cfg = config();

    static const uint8_t SIG_SEED[]    = { 0x89, 0x98, 0x54, 0x0B, 0x00, 0x00 };
    static const char    MSK_SEED[]    = "xxxxxx";
    int seed_hits = 0;
    uint8_t* seed_write = find_unique(mod, SIG_SEED, MSK_SEED, sizeof(SIG_SEED), &seed_hits);
    diagf("scan: seed-write    sig %d hit(s) @ %p", seed_hits, seed_write);

    if (seed_hits != 1) {
        diagf("FATAL: seed-write anchor not unique (%d hits) - mod inactive", seed_hits);
        logf("ERROR seed/state anchor unavailable; observation and locking are disabled");
        return 1;
    }

    uint8_t* p = seed_write;
    if (!(p[-7] == 0x48 && p[-6] == 0x8B && p[-5] == 0x05)) {
        diagf("FATAL: no 'mov rax,[rip+d]' before seed write (got %02X %02X %02X)",
             p[-7], p[-6], p[-5]);
        logf("ERROR state pointer anchor has an unexpected shape; mod is inactive");
        return 1;
    }
    uint8_t* state_slot = (uint8_t*)((p - 7) + 7 + *(int32_t*)(p - 4));
    diagf("state slot          = %p", state_slot);

    uint8_t* draw_call = p - 0x11;
    bool call_ok = (draw_call[0] == 0xFF && draw_call[1] == 0x15 &&
                    draw_call[6] == 0x48 && draw_call[7] == 0x8B && draw_call[8] == 0x5D);
    diagf("seed draw call site = %p (shape %s)", draw_call, call_ok ? "OK" : "UNEXPECTED");
    // The local the draw fills is named by the game's own `mov rbx,[rbp+disp8]`
    // right after the call, so the cave never has to assume a frame layout.
    // Layout of that instruction: 48 8B | 5D | <disp8> -> disp is at offset 9,
    // and modrm 0x5D is mod=01/rm=101, i.e. `[rbp+disp8]`.
    bool disp_ok = call_ok && (draw_call[8] & 0xC7) == 0x45;
    int8_t seed_disp8 = disp_ok ? (int8_t)draw_call[9] : 0;
    diagf("seed local         = [rbp%+d] (disp byte %02X, form %s)",
          (int)seed_disp8, draw_call[9], disp_ok ? "OK" : "UNEXPECTED");

    initialize_boss_hooks(mod);

    diagf("state fields: pattern=+0x%X seed=+0x%X", OFF_PATTERN, OFF_SEED);
    log_config_summary("READY");
    if (seed_value_valid(cfg.seed_setting) && !call_ok)
        logf("ERROR seed lock is unavailable on this game build; observation remains active");
    if (nightlord_id_valid(cfg.nightlord) && !nightlord_lock_available())
        logf("ERROR Nightlord lock is unavailable on this game build; observation remains active");
    if (everdark_value_valid(effective_everdark_value(cfg.nightlord, cfg.everdark)) &&
        !everdark_lock_available())
        logf("ERROR Everdark lock is unavailable on this game build; observation remains active");

    // ---- loop state ---------------------------------------------------------
    int32_t  last_pattern = INT32_MIN, last_nl = INT32_MIN;
    uint32_t last_seed    = 0;
    bool     have_last    = false;
    uint32_t last_ini_stamp = 0;
    DWORD    last_ini_check = 0;
    bool     seed_patched  = false;
    uint32_t applied_seed  = 0xDEADBEEFu;
    bool     logged_first_state = false;

    // cur_* is refreshed from the game each iteration.
    int32_t  cur_pat   = -1;
    uint32_t cur_seed  = 0;
    int32_t  cur_nl    = -1;
    LONG     last_direct_writes = -1;   // diagnostics for the direct writer
    LONG     last_seed_attempts = -1;
    LONG     last_seed_corrections = -1;
    LONG     last_nl_calls         = 0;
    LONG     last_nl_getter_corrections = 0;
    LONG     last_evd_corrections     = 0;
    bool     logged_no_seed_anchor = false;
    int32_t  roll_candidate_pattern = INT32_MIN;
    uint32_t roll_candidate_seed = 0;
    int      roll_stable_polls = 0;
    bool     roll_logged = false;
    LONG     last_getter_corrections_for_roll = 0;
    LONG     pending_raw_nightlord = -1;

    for (;;) {
        // --- ini hot reload ---------------------------------------------------
        DWORD now = GetTickCount();
        if (now - last_ini_check > 700) {
            last_ini_check = now;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(config_path(), GetFileExInfoStandard, &fad)) {
                uint32_t stamp = fad.ftLastWriteTime.dwLowDateTime ^ fad.ftLastWriteTime.dwHighDateTime;
                if (stamp != last_ini_stamp) {
                    last_ini_stamp = stamp;
                    Config before = cfg;
                    reload_config();
                    if (before.seed_setting != cfg.seed_setting ||
                        before.nightlord != cfg.nightlord ||
                        before.everdark != cfg.everdark ||
                        before.log != cfg.log ||
                        before.diag_range != cfg.diag_range) {
                        log_config_summary("CONFIG");
                        roll_logged = false;
                        roll_stable_polls = 0;
                    }
                }
            }
        }

        // --- 0. arm the Range() recorder on demand ---------------------------
        // Lazily, so `diag_range` can be switched on by editing the ini without
        // relaunching. It is a development aid; when it is off nothing at all is
        // scanned, patched or recorded.
        if (cfg.diag_range) install_range_diagnostics(mod);

        // --- 1. read the game's current state ---------------------------------
        uint8_t* obj = read_state_object(state_slot);
        if (obj) {
            cur_pat  = *(int32_t*)(obj + OFF_PATTERN);
            cur_seed = *(uint32_t*)(obj + OFF_SEED);
            cur_nl   = *(int32_t*)(obj + OFF_NIGHTLORD);

            bool changed = !have_last || cur_pat != last_pattern ||
                           cur_seed != last_seed || cur_nl != last_nl;
            if (changed) {
                if (!logged_first_state) {
                    // One-off dump of the whole parameter block so the meaning of
                    // each slot can be pinned down from a single sortie.
                    const int32_t* f = (const int32_t*)obj;
                    diagf("--- state dump (int32 at +0xB3C..+0xB7C) ---");
                    for (uint32_t off = 0xB3C; off <= 0xB7C; off += 4) {
                        diagf("    +0x%03X = %11d   (0x%08X)", off, f[off / 4], (uint32_t)f[off / 4]);
                    }
                    diagf("--- end dump ---");
                    diagf("first read: pattern=%d seed=0x%08X B4C=%d B50=%d B5C=%d",
                         cur_pat, cur_seed, cur_nl, *(int32_t*)(obj + OFF_NL_ALT),
                         *(int32_t*)(obj + 0xB5C));
                    logged_first_state = true;
                }
                last_pattern = cur_pat; last_seed = cur_seed; last_nl = cur_nl;
                have_last = true;
            }
        }

        // The game invalidates the pattern field (0xFFFFFFFF) while a sortie is
        // being set up, which is the natural start of a capture window: the draw
        // and the index pick both happen after it and before the ROLL line.
        if (range_diagnostics_installed() && (!obj || cur_pat < 0))
            reset_range_diagnostics();

        // One player-facing line per settled sortie. Waiting for three identical
        // polls filters the short seed transition that the corrector may replace.
        const BossHookSnapshot roll_hooks = boss_hook_snapshot();
        LONG getter_corrections = roll_hooks.nightlord.getter_corrections;
        if (getter_corrections != last_getter_corrections_for_roll) {
            last_getter_corrections_for_roll = getter_corrections;
            pending_raw_nightlord = roll_hooks.nightlord.getter_last_from;
        }
        bool zero_is_locked_seed = seed_value_valid(cfg.seed_setting) &&
                                   (uint32_t)cfg.seed_setting == 0;
        bool valid_roll = obj && cur_pat >= 0 && cur_pat <= 1199 &&
                          (cur_seed != 0 || zero_is_locked_seed);
        if (!valid_roll) {
            roll_stable_polls = 0;
            roll_logged = false;
        } else {
            if (cur_pat != roll_candidate_pattern || cur_seed != roll_candidate_seed) {
                roll_candidate_pattern = cur_pat;
                roll_candidate_seed = cur_seed;
                roll_stable_polls = 1;
                roll_logged = false;
            } else if (roll_stable_polls < 3) {
                ++roll_stable_polls;
            }
            if (roll_stable_polls >= 3 && !roll_logged) {
                LONG selected_nightlord = roll_hooks.nightlord.observed;
                bool nl_locked = nightlord_id_valid(cfg.nightlord) &&
                                 nightlord_lock_available();
                if (nl_locked) selected_nightlord = cfg.nightlord;

                // Variant: report the locked value only. With `everdark = -1`
                // nothing is written, and the byte the getter happened to read
                // last is NOT proof of what the run got: measured in game on
                // 2026-09-15, an unlocked run logged `variant=normal(rolled)`
                // (from `observed`) and its results page showed the Everdark
                // portrait - the getter also runs for menu objects, and the
                // run's own byte can be written after this line. The `VARIANT`
                // line in the diagnostics below reports an actual rewrite, which
                // is the signal that means something.
                int32_t evd_want = effective_everdark_value(
                    cfg.nightlord, cfg.everdark);   // -1 = no lock
                bool    evd_locked = everdark_lock_available() &&
                                     everdark_value_valid(evd_want);
                char    variant[32] = "";
                if (evd_locked)
                    _snprintf_s(variant, sizeof(variant), _TRUNCATE, " variant=%s",
                                evd_want ? "everdark" : "normal");

                if (selected_nightlord >= 0 && nl_locked && pending_raw_nightlord >= 0 &&
                    pending_raw_nightlord != selected_nightlord) {
                    logf("ROLL pattern=%d seed=0x%08X nightlord=%ld (rolled=%ld)%s",
                         cur_pat, cur_seed, selected_nightlord, pending_raw_nightlord,
                         variant);
                } else if (selected_nightlord >= 0) {
                    logf("ROLL pattern=%d seed=0x%08X nightlord=%ld%s",
                         cur_pat, cur_seed, selected_nightlord, variant);
                } else {
                    logf("ROLL pattern=%d seed=0x%08X%s", cur_pat, cur_seed, variant);
                }
                pending_raw_nightlord = -1;
                roll_logged = true;

                // Development-only dump of the Range() arguments captured for
                // this sortie. The weighted set lottery calls Range(0,total-1)
                // (large max) and the pattern-index draw calls Range(0,count-1)
                // (small max); the return address pins the exact call site, so
                // neither number has to be inferred from the static listing.
                log_range_diagnostics(mod);
            }
        }

        // --- 2. decide what to force ------------------------------------------
        bool want_seed = seed_value_valid(cfg.seed_setting) && call_ok;
        // Release policy: pattern is observation-only. Seed remains the single
        // source of truth for both the map layout and its spawn point.
        uint32_t seed_to_force = seed_value_valid(cfg.seed_setting)
                               ? (uint32_t)cfg.seed_setting : 0;

        // Publish the forced byte only after the value is ready. Turning the lock
        // off happens first, so a hot reload can never expose a stale value.
        verify_boss_hooks();
        // The item stores the base boss id unchanged, so the configured value is
        // written as-is. Anything outside 0..9 was rejected by
        // nightlord_id_valid() and leaves the boss unlocked.
        bool force_nl = nightlord_lock_available() &&
                        nightlord_id_valid(cfg.nightlord);
        set_nightlord_lock(force_nl, cfg.nightlord);

        // Variant lock: 0 forces the normal boss, 1 forces its Everdark variant,
        // and -1 leaves whatever the game rolled. The exception is a locked
        // Heolstor (7) or Straghess (9): neither has an Everdark form, so normal
        // is forced regardless of the configured value.
        int32_t evd_want = effective_everdark_value(cfg.nightlord, cfg.everdark);
        bool force_evd = everdark_lock_available() && everdark_value_valid(evd_want);
        set_everdark_lock(force_evd, evd_want);

        // --- 3. apply the seed lock -------------------------------------------
        // Two mechanisms, used together:
        //   (a) the cave that fills the roll's seed local. `ebx` feeds both the
        //       weighted set lottery (0x6AEDC1) and the index draw (0x6AEE68),
        //       so this is the only thing that actually decides the pattern;
        //   (b) a direct write of +0xB54 keeps the reported/state seed in sync.
        if (want_seed) {
            if (!seed_patched || applied_seed != seed_to_force) {
                seed_patched = disp_ok && install_seed_stub(draw_call, seed_disp8);
                if (seed_patched && !apply_seed_stub(seed_to_force)) {
                    seed_patched = false;
                }
                if (!seed_patched && !logged_no_seed_anchor) {
                    logged_no_seed_anchor = true;
                    diagf("SEED: seed-local cave unavailable - relying on direct writes");
                }
            }
            if (applied_seed != seed_to_force) {
                applied_seed = seed_to_force;
                diagf("SEED LOCK ON  -> 0x%08X (seed-local cave %s + direct write)",
                     seed_to_force, seed_patched ? "applied" : "UNPATCHED");
                if (seed_patched)
                    logf("LOCK seed=0x%08X", seed_to_force);
                else
                    logf("ERROR seed lock could not patch the draw site");
            }
            // Correct the field whenever the game has put something else there.
            if (obj && *(uint32_t*)(obj + OFF_SEED) != seed_to_force &&
                (cur_seed != 0 || cur_pat >= 0)) {
                direct_write_i32(obj, OFF_SEED, (int32_t)seed_to_force);
                InterlockedIncrement(&g_direct_writes);
            }
            InterlockedIncrement(&g_seed_attempts);

            // Hand the field to the dedicated corrector. It is armed exactly when
            // the lock is, and its target follows the configured seed. The
            // worker's own correction above stays as a belt-and-braces check that
            // also covers the first iteration, before the thread has woken up.
            update_seed_corrector(obj, seed_to_force);
            {
                const SeedDiagnostics diagnostics = seed_diagnostics();
                LONG cor = diagnostics.corrections;
                if (cor != last_seed_corrections) {
                    last_seed_corrections = cor;
                    char hist[320];
                    int hn = format_seed_history(hist, sizeof(hist));
                    diagf("SEED RACE: %ld correction(s) / %ld foreign value(s) seen"
                         " / %ld checks; target=0x%08X field=0x%08X",
                         cor, diagnostics.observations, diagnostics.checks,
                         seed_to_force, cur_seed);
                    if (hn) diagf("SEED RACE history: %s", hist);
                }
            }
        } else {
            disable_seed_corrector();
            if (seed_patched) {
                disable_seed_stub();
                if (!seed_stub_installed()) {
                    seed_patched = false;
                    diagf("SEED LOCK OFF");
                    logf("LOCK seed=off");
                }
            }
        }

        // --- 4. diagnostics ---------------------------------------------------
        // Report seed correction activity only in a verbose research build.
        {
            LONG dw = InterlockedCompareExchange(&g_direct_writes, 0, 0);
            if (dw != last_direct_writes) {
                last_direct_writes = dw;
                diagf("DIRECT SEED WRITES so far: %ld; last observed seed=0x%08X",
                     dw, cur_seed);
            }
            // Log the seed check only when it carries information: the first one,
            // then a heartbeat every ~2000 checks. The loop runs 50x/second, so an
            // ungated line here costs about 10 MB of log per hour - and, worse,
            // buries the lines that matter.
            LONG sa = InterlockedCompareExchange(&g_seed_attempts, 0, 0);
            bool drift = (obj && *(uint32_t*)(obj + OFF_SEED) != seed_to_force);
            if (sa != last_seed_attempts &&
                (drift || last_seed_attempts < 0 || (sa % 2000) == 0)) {
                last_seed_attempts = sa;
                if (drift)
                    diagf("SEED: field drifted to 0x%08X (target 0x%08X) - corrected",
                         cur_seed, seed_to_force);
                else
                    diagf("SEED check #%ld: field=0x%08X target=0x%08X HELD",
                         sa, cur_seed, seed_to_force);
            }
        }

        // --- 5. Nightlord and variant hook diagnostics ------------------------
        const BossHookSnapshot latest_hooks = boss_hook_snapshot();
        {
            const LONG corrections = latest_hooks.nightlord.getter_corrections;
            if (corrections != last_nl_getter_corrections) {
                const LONG delta = corrections - last_nl_getter_corrections;
                last_nl_getter_corrections = corrections;
                void* source = latest_hooks.nightlord.source_object;
                const LONG source_value =
                    readable_bytes(source ? static_cast<uint8_t*>(source) +
                                                OFF_NL_SOURCE
                                          : nullptr,
                                   1)
                        ? *(static_cast<uint8_t*>(source) + OFF_NL_SOURCE)
                        : -1;
                diagf("NIGHTLORD GETTER: game=%ld -> forced=%ld source-field=%ld "
                      "(correction #%ld, +%ld; getter call #%ld) source=%p",
                      latest_hooks.nightlord.getter_last_from,
                      latest_hooks.nightlord.value, source_value, corrections,
                      delta, latest_hooks.nightlord.getter_calls, source);
            }

            const LONG calls = latest_hooks.nightlord.calls;
            if (calls != last_nl_calls) {
                last_nl_calls = calls;
                void* source = latest_hooks.nightlord.source_object;
                void* vtable =
                    readable_bytes(source, sizeof(void*)) ? *static_cast<void**>(source)
                                                          : nullptr;
                void* getter_slot =
                    vtable ? static_cast<uint8_t*>(vtable) + 0x188 : nullptr;
                void* getter = readable_bytes(getter_slot, sizeof(void*))
                                   ? *static_cast<void**>(getter_slot)
                                   : nullptr;
                void* source_slot =
                    source ? static_cast<uint8_t*>(source) + OFF_NL_SOURCE
                           : nullptr;
                const LONG source_value =
                    readable_bytes(source_slot, 1)
                        ? *static_cast<uint8_t*>(source_slot)
                        : -1;
                if (latest_hooks.nightlord.enabled)
                    diagf("NIGHTLORD ARG: map=%ld -> forced=%ld source-field=%ld "
                          "(call #%ld; getter raw=%ld calls=%ld corrections=%ld) "
                          "source=%p vtable=%p getter=%p",
                          latest_hooks.nightlord.observed,
                          latest_hooks.nightlord.value, source_value, calls,
                          latest_hooks.nightlord.getter_observed,
                          latest_hooks.nightlord.getter_calls,
                          latest_hooks.nightlord.getter_corrections, source,
                          vtable, getter);
                else
                    diagf("NIGHTLORD ARG: map=%ld source-field=%ld (observed, call #%ld; "
                          "getter raw=%ld calls=%ld corrections=%ld) source=%p vtable=%p getter=%p",
                          latest_hooks.nightlord.observed, source_value, calls,
                          latest_hooks.nightlord.getter_observed,
                          latest_hooks.nightlord.getter_calls,
                          latest_hooks.nightlord.getter_corrections, source,
                          vtable, getter);
            }
        }

        {
            const LONG corrections = latest_hooks.everdark.corrections;
            if (corrections != last_evd_corrections) {
                const LONG delta = corrections - last_evd_corrections;
                last_evd_corrections = corrections;
                diagf("EVERDARK GETTER: game=%ld -> forced=%ld (correction #%ld, +%ld;"
                      " getter call #%ld) source=%p",
                      latest_hooks.everdark.last_from,
                      latest_hooks.everdark.value, corrections, delta,
                      latest_hooks.everdark.calls,
                      latest_hooks.everdark.source_object);
                if (latest_hooks.everdark.last_from !=
                    latest_hooks.everdark.value)
                    logf("VARIANT %s (game rolled %s)",
                         latest_hooks.everdark.value ? "everdark" : "normal",
                         latest_hooks.everdark.last_from ? "everdark" : "normal");
            }
        }

        Sleep(20);
    }
}

}  // namespace derandomizer::mod

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
#if !defined(DERANDOMIZER_EMIT_ONLY)
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    using namespace derandomizer::mod;

    initialize_logging(self);
    initialize_config(self);
    const Config& cfg = config();
    logf("=== derandomizer loaded ===");
    diagf("=== derandomizer loaded ===");
    diagf("build  : %s %s", __DATE__, __TIME__);
    diagf("ini    : %ls", config_path());
    diagf("config : seed=%lld nightlord=%d everdark=%d log=%d",
         (long long)cfg.seed_setting, cfg.nightlord, cfg.everdark, (int)cfg.log);
    if (cfg.nightlord != -1 && !nightlord_id_valid(cfg.nightlord))
        diagf("config : nightlord=%d is not -1 or 0..9; boss lock disabled",
             cfg.nightlord);
    if (nightlord_has_no_everdark(cfg.nightlord) && cfg.everdark != 0)
        diagf("config : nightlord=%d has no Everdark variant; everdark=%d ignored, effective=0",
              cfg.nightlord, cfg.everdark);
    else if (cfg.everdark != -1 && !everdark_value_valid(cfg.everdark))
        diagf("config : everdark=%d is not -1, 0 or 1; variant lock disabled",
             cfg.everdark);

    static ModuleInfo mod{};
    if (!get_main_module(mod)) {
        logf("ERROR main game module was not found; mod is inactive");
        return TRUE;
    }
    read_build_id(mod, g_build);
    diagf("main   : base=%p size=0x%zX", mod.base, mod.size);
    log_build_banner(g_build);
    if (!build_matches(g_build))
        logf("WARN game version %s differs from verified %s; unsafe anchors will be refused",
             g_build.file_version[0] ? g_build.file_version : "unknown",
             verified_file_version());

    // Log a breadcrumb if anything faults, so a crash report is actionable.
    install_crash_filter();

    start_seed_corrector();

    HANDLE t = CreateThread(nullptr, 0, worker, &mod, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}

// me3 loads natives with LoadLibrary, so no specific export is required. This
// one exists only so the DLL is also usable as a dinput8 proxy if ever needed.
extern "C" __declspec(dllexport) void* __stdcall derandomizer_dinput8_stub() {
    return nullptr;
}
#endif
