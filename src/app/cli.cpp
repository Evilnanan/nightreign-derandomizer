#define NOMINMAX
#include "cli.h"

#include "config.h"
#include "model.h"

#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>

namespace seed_reroller {
namespace {

struct Options {
    bool deepOfNight = true;
    int everdark = -1;
    std::optional<int> nightlord;
    std::optional<std::filesystem::path> configPath;
    bool json = false;
};

void printHelp() {
    std::wcout
        << L"Seed Reroller - Nightreign seed and pattern utility\n\n"
        << L"Usage:\n"
        << L"  Seed Reroller.exe                         Open the graphical interface\n"
        << L"  Seed Reroller.exe find <pattern> [options]\n"
        << L"  Seed Reroller.exe predict <seed> --nightlord <0-9> [options]\n\n"
        << L"Options:\n"
        << L"  --mode <deep|normal>                     Expedition mode (default: deep)\n"
        << L"  --variant <auto|normal|everdark>         INI variant (default: auto)\n"
        << L"  --config <path>                          Write derandomizer.ini\n"
        << L"  --json                                   Emit one JSON object\n"
        << L"  -h, --help                               Show this help\n\n"
        << L"Examples:\n"
        << L"  Seed Reroller.exe find 1042 --config derandomizer.ini\n"
        << L"  Seed Reroller.exe predict 0x066BB95B --nightlord 7 --mode deep\n"
        << L"  Seed Reroller.exe find 1042 --json\n";
}

std::wstring lower(std::wstring value) {
    for (wchar_t& character : value) character = static_cast<wchar_t>(towlower(character));
    return value;
}

bool parseUnsigned32(const std::wstring& text, std::uint32_t& value) {
    const wchar_t* begin = text.c_str();
    while (*begin == L' ' || *begin == L'\t') ++begin;
    if (*begin == L'\0' || *begin == L'-') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = wcstoull(begin, &end, 0);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (errno == ERANGE || !end || *end != L'\0' || parsed > 0xFFFFFFFFull) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parseInteger(const std::wstring& text, int& value) {
    const wchar_t* begin = text.c_str();
    while (*begin == L' ' || *begin == L'\t') ++begin;
    if (*begin == L'\0') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = wcstol(begin, &end, 10);
    while (end && (*end == L' ' || *end == L'\t')) ++end;
    if (errno == ERANGE || !end || *end != L'\0') return false;
    value = static_cast<int>(parsed);
    return true;
}

std::wstring hex32(std::uint32_t value) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08X", value);
    return buffer;
}

std::uint32_t randomUint32() {
    std::random_device device;
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return (static_cast<std::uint32_t>(device()) << 16) ^
           static_cast<std::uint32_t>(device()) ^
           static_cast<std::uint32_t>(ticks.QuadPart) ^ GetTickCount();
}

std::pair<std::uint32_t, std::uint32_t> randomPath() {
    return {randomUint32(), randomUint32() | 1u};
}

int effectiveEverdark(int nightlord, int configured) {
    return nightlord == 7 || nightlord == 9 ? 0 : configured;
}

std::wstring jsonEscape(const std::wstring& value) {
    std::wostringstream output;
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': output << L"\\\""; break;
        case L'\\': output << L"\\\\"; break;
        case L'\b': output << L"\\b"; break;
        case L'\f': output << L"\\f"; break;
        case L'\n': output << L"\\n"; break;
        case L'\r': output << L"\\r"; break;
        case L'\t': output << L"\\t"; break;
        default: output << character; break;
        }
    }
    return output.str();
}

bool parseOptions(int argc, wchar_t* argv[], int start, Options& options,
                  std::wstring& error) {
    for (int index = start; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto requireValue = [&](const wchar_t* name) -> const wchar_t* {
            if (++index < argc) return argv[index];
            error = std::wstring(name) + L" requires a value";
            return nullptr;
        };

        if (argument == L"--mode") {
            const wchar_t* raw = requireValue(L"--mode");
            if (!raw) return false;
            const std::wstring value = lower(raw);
            if (value == L"deep" || value == L"deep-of-night") options.deepOfNight = true;
            else if (value == L"normal") options.deepOfNight = false;
            else {
                error = L"--mode must be deep or normal";
                return false;
            }
        } else if (argument == L"--variant") {
            const wchar_t* raw = requireValue(L"--variant");
            if (!raw) return false;
            const std::wstring value = lower(raw);
            if (value == L"auto") options.everdark = -1;
            else if (value == L"normal") options.everdark = 0;
            else if (value == L"everdark") options.everdark = 1;
            else {
                error = L"--variant must be auto, normal or everdark";
                return false;
            }
        } else if (argument == L"--nightlord") {
            const wchar_t* raw = requireValue(L"--nightlord");
            int nightlord = -1;
            if (!raw || !parseInteger(raw, nightlord) || nightlord < 0 || nightlord > 9) {
                error = L"--nightlord must be an integer from 0 to 9";
                return false;
            }
            options.nightlord = nightlord;
        } else if (argument == L"--config") {
            const wchar_t* raw = requireValue(L"--config");
            if (!raw || *raw == L'\0') {
                error = L"--config requires a non-empty path";
                return false;
            }
            options.configPath = std::filesystem::path(raw);
        } else if (argument == L"--json") {
            options.json = true;
        } else {
            error = L"Unknown option: " + argument;
            return false;
        }
    }
    return true;
}

bool writeConfigIfRequested(const Options& options, std::uint32_t seed,
                            int nightlord, std::wstring& error) {
    if (!options.configPath) return true;
    return derandomizer::writeModConfig(*options.configPath, seed, nightlord,
                                        options.everdark, error);
}

void printResult(const Options& options, std::uint32_t seed, int pattern,
                 int nightlord, const derandomizer::Prediction& prediction) {
    const std::wstring mode = options.deepOfNight ? L"deep" : L"normal";
    const int everdark = effectiveEverdark(nightlord, options.everdark);
    if (options.json) {
        std::wcout << L"{\"seed\":\"" << hex32(seed)
                   << L"\",\"pattern\":" << pattern
                   << L",\"nightlord\":" << nightlord
                   << L",\"nightlordName\":\""
                   << jsonEscape(derandomizer::nightlordName(nightlord))
                   << L"\",\"mode\":\"" << mode
                   << L"\",\"block\":\"" << jsonEscape(prediction.block)
                   << L"\",\"draw\":" << prediction.draw
                   << L",\"everdark\":" << everdark;
        if (options.configPath) {
            std::wcout << L",\"config\":\""
                       << jsonEscape(options.configPath->wstring()) << L"\"";
        }
        std::wcout << L"}\n";
        return;
    }

    std::wcout << L"seed=" << hex32(seed) << L"\n"
               << L"pattern=" << pattern << L"\n"
               << L"nightlord=" << nightlord << L"\n"
               << L"nightlord_name=" << derandomizer::nightlordName(nightlord) << L"\n"
               << L"mode=" << mode << L"\n"
               << L"block=" << prediction.block << L"\n"
               << L"draw=" << prediction.draw << L"\n"
               << L"everdark=" << everdark << L"\n";
    if (options.configPath) std::wcout << L"config=" << options.configPath->wstring() << L"\n";
}

int fail(const std::wstring& message, bool showUsage = false) {
    std::wcerr << L"Seed Reroller: " << message << L"\n";
    if (showUsage) std::wcerr << L"Run 'Seed Reroller.exe --help' for usage.\n";
    return 2;
}

int runFind(const std::wstring& input, const Options& options) {
    int pattern = -1;
    if (!parseInteger(input, pattern) || pattern < 0 || pattern > 1199) {
        return fail(L"pattern must be an integer from 0 to 1199");
    }
    const auto nightlord = derandomizer::nightlordOf(pattern);
    if (!nightlord) return fail(L"pattern is unassigned");
    if (!derandomizer::isReachable(pattern, options.deepOfNight)) {
        return fail(L"pattern is unavailable in the selected mode");
    }

    const auto [start, stride] = randomPath();
    const auto found = derandomizer::findSeedRandom(pattern, options.deepOfNight,
                                                    start, stride);
    if (!found) return fail(L"no seed found");

    std::wstring error;
    if (!writeConfigIfRequested(options, found->seed, *nightlord, error)) {
        return fail(error);
    }
    printResult(options, found->seed, pattern, *nightlord, found->prediction);
    return 0;
}

int runPredict(const std::wstring& input, const Options& options) {
    std::uint32_t seed = 0;
    if (!parseUnsigned32(input, seed)) {
        return fail(L"seed must be an integer from 0 to 0xFFFFFFFF");
    }
    if (!options.nightlord) return fail(L"predict requires --nightlord <0-9>");

    const auto prediction = derandomizer::predict(*options.nightlord,
                                                   options.deepOfNight, seed);
    if (!prediction) return fail(L"could not calculate a pattern");

    std::wstring error;
    if (!writeConfigIfRequested(options, seed, *options.nightlord, error)) {
        return fail(error);
    }
    printResult(options, seed, prediction->pattern, *options.nightlord, *prediction);
    return 0;
}

}  // namespace

int runCli(int argc, wchar_t* argv[]) {
    if (argc == 2 && (std::wstring(argv[1]) == L"--help" ||
                      std::wstring(argv[1]) == L"-h")) {
        printHelp();
        return 0;
    }
    if (argc == 3 && (std::wstring(argv[2]) == L"--help" ||
                      std::wstring(argv[2]) == L"-h")) {
        printHelp();
        return 0;
    }
    if (argc < 3) return fail(L"missing command or input", true);

    const std::wstring command = lower(argv[1]);
    if (command != L"find" && command != L"predict") {
        return fail(L"unknown command: " + std::wstring(argv[1]), true);
    }

    Options options;
    std::wstring error;
    if (!parseOptions(argc, argv, 3, options, error)) return fail(error, true);
    if (command == L"find" && options.nightlord) {
        return fail(L"find determines the Nightlord from the pattern; remove --nightlord");
    }
    return command == L"find" ? runFind(argv[2], options)
                               : runPredict(argv[2], options);
}

}  // namespace seed_reroller
