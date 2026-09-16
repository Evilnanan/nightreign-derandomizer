#include "config.h"

#include "logging.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace derandomizer::mod {
namespace {

Config g_config;
wchar_t g_config_path[MAX_PATH]{};

void read_ini_value(const char* key, const char* default_value,
                    char* output, size_t output_size) {
    wchar_t wide_key[128];
    wchar_t wide_default[128];
    wchar_t wide_value[512];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wide_key, 128);
    MultiByteToWideChar(CP_UTF8, 0, default_value, -1, wide_default, 128);
    wide_value[0] = 0;
    GetPrivateProfileStringW(L"settings", wide_key, wide_default, wide_value,
                             512, g_config_path);
    WideCharToMultiByte(CP_UTF8, 0, wide_value, -1, output,
                        static_cast<int>(output_size), nullptr, nullptr);
    output[output_size - 1] = 0;
}

}  // namespace

void initialize_config(HMODULE self) {
    wchar_t directory[MAX_PATH];
    GetModuleFileNameW(self, directory, MAX_PATH);
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (slash) *(slash + 1) = 0;
    _snwprintf_s(g_config_path, MAX_PATH, _TRUNCATE,
                 L"%sderandomizer.ini", directory);
    reload_config();
}

void reload_config() {
    Config loaded{};
    char value[128];

    read_ini_value("nightlord", "-1", value, sizeof(value));
    loaded.nightlord = static_cast<std::int32_t>(std::strtol(value, nullptr, 0));

    read_ini_value("everdark", "-1", value, sizeof(value));
    loaded.everdark = static_cast<std::int32_t>(std::strtol(value, nullptr, 0));

    read_ini_value("seed", "-1", value, sizeof(value));
    loaded.seed_setting = _strtoi64(value, nullptr, 0);

    read_ini_value("log", "0", value, sizeof(value));
    loaded.log = std::atoi(value) != 0;

    read_ini_value("diag_range", "0", value, sizeof(value));
    loaded.diag_range = std::atoi(value) != 0;

    g_config = loaded;
    set_logging_enabled(loaded.log);
}

const Config& config() {
    return g_config;
}

const wchar_t* config_path() {
    return g_config_path;
}

bool nightlord_id_valid(std::int32_t id) {
    return id >= 0 && id <= 9;
}

bool seed_value_valid(std::int64_t seed) {
    return seed >= 0 && static_cast<std::uint64_t>(seed) <= UINT32_MAX;
}

bool everdark_value_valid(std::int32_t value) {
    return value == 0 || value == 1;
}

bool nightlord_has_no_everdark(std::int32_t nightlord) {
    return nightlord == 7 || nightlord == 9;
}

std::int32_t effective_everdark_value(std::int32_t nightlord,
                                      std::int32_t configured) {
    return nightlord_has_no_everdark(nightlord) ? 0 : configured;
}

void log_config_summary(const char* event) {
    char seed[48];
    char nightlord[48];
    char everdark[48];
    const std::int32_t effective_everdark =
        effective_everdark_value(g_config.nightlord, g_config.everdark);

    if (g_config.seed_setting == -1)
        strcpy_s(seed, "off");
    else if (seed_value_valid(g_config.seed_setting))
        _snprintf_s(seed, sizeof(seed), _TRUNCATE, "0x%08X",
                    static_cast<std::uint32_t>(g_config.seed_setting));
    else
        _snprintf_s(seed, sizeof(seed), _TRUNCATE, "invalid(%lld)",
                    static_cast<long long>(g_config.seed_setting));

    if (g_config.nightlord == -1)
        strcpy_s(nightlord, "off");
    else if (nightlord_id_valid(g_config.nightlord))
        _snprintf_s(nightlord, sizeof(nightlord), _TRUNCATE, "%d",
                    g_config.nightlord);
    else
        _snprintf_s(nightlord, sizeof(nightlord), _TRUNCATE, "invalid(%d)",
                    g_config.nightlord);

    if (nightlord_has_no_everdark(g_config.nightlord)) {
        strcpy_s(everdark, "0(auto)");
    } else if (everdark_value_valid(effective_everdark)) {
        _snprintf_s(everdark, sizeof(everdark), _TRUNCATE, "%d",
                    effective_everdark);
    } else if (effective_everdark == -1) {
        strcpy_s(everdark, "off");
    } else {
        _snprintf_s(everdark, sizeof(everdark), _TRUNCATE, "invalid(%d)",
                    g_config.everdark);
    }

    if (g_config.diag_range)
        logf("%s seed=%s nightlord=%s everdark=%s log=%d diag_range=1",
             event, seed, nightlord, everdark, static_cast<int>(g_config.log));
    else
        logf("%s seed=%s nightlord=%s everdark=%s log=%d",
             event, seed, nightlord, everdark, static_cast<int>(g_config.log));

    if (g_config.seed_setting != -1 && !seed_value_valid(g_config.seed_setting))
        logf("ERROR seed must be -1 or a 32-bit value");
    if (g_config.nightlord != -1 && !nightlord_id_valid(g_config.nightlord))
        logf("ERROR nightlord must be -1 or 0..9 (10..16/18 are Everdark rows: "
             "use nightlord=0..9 plus everdark=1)");
    if (nightlord_has_no_everdark(g_config.nightlord) && g_config.everdark != 0)
        logf("POLICY nightlord=%d has no Everdark variant; everdark=%d ignored and normal forced",
             g_config.nightlord, g_config.everdark);
    else if (g_config.everdark != -1 && !everdark_value_valid(g_config.everdark))
        logf("ERROR everdark must be -1, 0 or 1");
}

}  // namespace derandomizer::mod
