#include "game_module.h"

#include "logging.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace derandomizer::mod {
namespace {

constexpr char kVerifiedFileVersion[] = "1.3.3.0";
constexpr DWORD kVerifiedTimestamp = 0x69E0DC2A;
constexpr DWORD kVerifiedImageSize = 0x7C5D600;

void get_main_version(char* output, std::size_t output_length) {
    output[0] = 0;
    char executable[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, executable, MAX_PATH)) return;
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeA(executable, &unused);
    if (!size) return;

    BYTE* buffer = static_cast<BYTE*>(std::malloc(size));
    if (!buffer) return;
    if (GetFileVersionInfoA(executable, 0, size, buffer)) {
        VS_FIXEDFILEINFO* version = nullptr;
        UINT length = 0;
        if (VerQueryValueA(buffer, "\\", reinterpret_cast<void**>(&version),
                           &length) && version) {
            _snprintf_s(output, output_length, _TRUNCATE, "%u.%u.%u.%u",
                        HIWORD(version->dwFileVersionMS),
                        LOWORD(version->dwFileVersionMS),
                        HIWORD(version->dwFileVersionLS),
                        LOWORD(version->dwFileVersionLS));
        }
    }
    std::free(buffer);
}

}  // namespace

bool get_main_module(ModuleInfo& output) {
    HMODULE handle = GetModuleHandleA(nullptr);
    if (!handle) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(handle);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        reinterpret_cast<std::uint8_t*>(handle) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    output.base = reinterpret_cast<std::uint8_t*>(handle);
    output.size = nt->OptionalHeader.SizeOfImage;
    return true;
}

void read_build_id(const ModuleInfo& module, BuildId& output) {
    get_main_version(output.file_version, sizeof(output.file_version));
    output.size_of_image = static_cast<DWORD>(module.size);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    output.time_date_stamp = nt->FileHeader.TimeDateStamp;
}

bool build_matches(const BuildId& build) {
    return _stricmp(build.file_version, kVerifiedFileVersion) == 0;
}

const char* verified_file_version() {
    return kVerifiedFileVersion;
}

void log_build_banner(const BuildId& build) {
    const bool timestamp_matches = build.time_date_stamp == kVerifiedTimestamp;
    const bool size_matches = build.size_of_image == kVerifiedImageSize;

    diagf("game   : file-version=%s time-date-stamp=0x%08X size-of-image=0x%X",
          build.file_version[0] ? build.file_version : "(unknown)",
          build.time_date_stamp, build.size_of_image);
    diagf("dll    : verified against file-version=%s time-date-stamp=0x%08X size-of-image=0x%X",
          kVerifiedFileVersion, kVerifiedTimestamp, kVerifiedImageSize);

    if (build_matches(build) && timestamp_matches && size_matches) {
        diagf("build  : MATCH - anchors were derived from this exact build");
    } else if (build_matches(build)) {
        diagf("build  : version MATCH but stamp/size differ (stamp_ok=%d size_ok=%d) - "
              "rebuilt binary; anchors are probably still fine",
              static_cast<int>(timestamp_matches), static_cast<int>(size_matches));
    } else {
        diagf("build  : *** VERSION MISMATCH ***");
        diagf("         this DLL was verified against %s but the game reports %s.",
              kVerifiedFileVersion,
              build.file_version[0] ? build.file_version : "(unknown)");
        diagf("         Each anchor's *shape* is still checked before anything is patched,");
        diagf("         so an unrecognised build is refused rather than corrupted - but a");
        diagf("         game update can change meaning without changing bytes. Re-run");
        diagf("         re-verify all signature anchors before enabling either lock.");
    }
}

std::uint8_t* find_unique(const ModuleInfo& module,
                          const std::uint8_t* signature, const char* mask,
                          std::size_t length, int* hits_output) {
    int hits = 0;
    std::uint8_t* found = nullptr;
    if (module.size < length) {
        if (hits_output) *hits_output = 0;
        return nullptr;
    }
    for (std::size_t i = 0; i + length <= module.size; ++i) {
        bool matches = true;
        for (std::size_t j = 0; j < length; ++j) {
            if (mask[j] == 'x' && module.base[i + j] != signature[j]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (!found) found = module.base + i;
            ++hits;
        }
    }
    if (hits_output) *hits_output = hits;
    return found;
}

int find_all(const ModuleInfo& module, const std::uint8_t* signature,
             const char* mask, std::size_t length, std::uint8_t** output,
             int output_capacity) {
    int hits = 0;
    if (module.size < length) return 0;
    for (std::size_t i = 0; i + length <= module.size; ++i) {
        bool matches = true;
        for (std::size_t j = 0; j < length; ++j) {
            if (mask[j] == 'x' && module.base[i + j] != signature[j]) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (hits < output_capacity) output[hits] = module.base + i;
        ++hits;
    }
    return hits;
}

}  // namespace derandomizer::mod

