#include "logging.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace derandomizer::mod {
namespace {

CRITICAL_SECTION g_log_mutex;
bool g_log_ready = false;
wchar_t g_log_path[MAX_PATH]{};
volatile LONG g_log_enabled = 0;

void write_log_line(const char* format, va_list arguments) {
    if (!g_log_ready || !InterlockedCompareExchange(&g_log_enabled, 0, 0)) return;

    char line[1024];
    SYSTEMTIME time;
    GetLocalTime(&time);
    int prefix_length = _snprintf_s(
        line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] ",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    if (prefix_length < 0) prefix_length = 0;
    _vsnprintf_s(line + prefix_length, sizeof(line) - static_cast<size_t>(prefix_length),
                 _TRUNCATE, format, arguments);

    size_t length = std::strlen(line);
    if (length < sizeof(line) - 2) {
        line[length++] = '\r';
        line[length++] = '\n';
        line[length] = 0;
    }

    EnterCriticalSection(&g_log_mutex);
    HANDLE file = CreateFileW(g_log_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        SetFilePointer(file, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
    LeaveCriticalSection(&g_log_mutex);
}

}  // namespace

void initialize_logging(HMODULE self) {
    wchar_t directory[MAX_PATH];
    GetModuleFileNameW(self, directory, MAX_PATH);
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (slash) *slash = 0;
    _snwprintf_s(g_log_path, MAX_PATH, _TRUNCATE,
                 L"%s\\derandomizer.log", directory);
    InitializeCriticalSection(&g_log_mutex);

    // Rotate rather than append forever. Rotation happens before worker threads
    // are created, so no writer can still hold the old file open here.
    HANDLE file = CreateFileW(g_log_path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > (32LL << 20)) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
            wchar_t old_path[MAX_PATH];
            _snwprintf_s(old_path, MAX_PATH, _TRUNCATE, L"%s.1", g_log_path);
            DeleteFileW(old_path);
            MoveFileW(g_log_path, old_path);
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    g_log_ready = true;
}

void set_logging_enabled(bool enabled) {
    InterlockedExchange(&g_log_enabled, enabled ? 1 : 0);
}

void diagf(const char* format, ...) {
#if defined(DERANDOMIZER_EMIT_ONLY)
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, arguments);
    va_end(arguments);
    std::printf("[stub] %s\n", buffer);
#elif !defined(DERANDOMIZER_VERBOSE_LOG)
    (void)format;
#else
    va_list arguments;
    va_start(arguments, format);
    write_log_line(format, arguments);
    va_end(arguments);
#endif
}

void logf(const char* format, ...) {
#if defined(DERANDOMIZER_EMIT_ONLY)
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, arguments);
    va_end(arguments);
    std::printf("[stub] %s\n", buffer);
#else
    va_list arguments;
    va_start(arguments, format);
    write_log_line(format, arguments);
    va_end(arguments);
#endif
}

}  // namespace derandomizer::mod
