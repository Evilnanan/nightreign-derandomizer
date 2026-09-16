#define NOMINMAX
#include "model.h"
#include "config.h"
#include "cli.h"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"DerandomizerSeedRerollerWindow";
constexpr COLORREF kBackground = RGB(13, 16, 22);
constexpr COLORREF kCard = RGB(22, 27, 36);
constexpr COLORREF kCardRaised = RGB(28, 34, 44);
constexpr COLORREF kBorder = RGB(48, 57, 71);
constexpr COLORREF kText = RGB(241, 244, 248);
constexpr COLORREF kMuted = RGB(148, 158, 174);
constexpr COLORREF kAccent = RGB(104, 224, 205);
constexpr COLORREF kAccentDark = RGB(21, 66, 61);

enum ControlId : int {
    ID_TAB_PATTERN = 100,
    ID_TAB_SEED,
    ID_INPUT_LABEL,
    ID_INPUT,
    ID_MODE_LABEL,
    ID_MODE,
    ID_NIGHTLORD_LABEL,
    ID_NIGHTLORD,
    ID_NIGHTLORD_VALUE,
    ID_ACTION,
    ID_COPY,
    ID_RESULT,
    ID_CONFIG_LABEL,
    ID_CONFIG_PATH,
    ID_BROWSE,
    ID_VARIANT_LABEL,
    ID_VARIANT,
    ID_WRITE_CONFIG,
    ID_STATUS,
};

enum class View { PatternToSeed, SeedToPattern };

struct AppState {
    HWND window{};
    UINT dpi = 96;
    View view = View::PatternToSeed;
    bool suppressChanges = false;
    std::wstring patternText;
    std::wstring seedText;
    std::optional<std::uint32_t> activeSeed;
    int activeNightlord = -1;

    HFONT labelFont{};
    HFONT bodyFont{};
    HFONT monoFont{};
    HFONT buttonFont{};
    HBRUSH backgroundBrush{};
    HBRUSH cardBrush{};
    HBRUSH editBrush{};
    WNDPROC comboWindowProc{};

    HWND tabPattern{};
    HWND tabSeed{};
    HWND inputLabel{};
    HWND input{};
    HWND modeLabel{};
    HWND mode{};
    HWND nightlordLabel{};
    HWND nightlord{};
    HWND nightlordValue{};
    HWND action{};
    HWND copy{};
    HWND result{};
    HWND configLabel{};
    HWND configPath{};
    HWND browse{};
    HWND variantLabel{};
    HWND variant{};
    HWND writeConfig{};
    HWND status{};
} g;

int scale(int value) {
    return MulDiv(value, static_cast<int>(g.dpi), 96);
}

void deleteFonts() {
    for (HFONT font : {g.labelFont, g.bodyFont, g.monoFont, g.buttonFont}) {
        if (font) DeleteObject(font);
    }
    g.labelFont = g.bodyFont = nullptr;
    g.monoFont = g.buttonFont = nullptr;
}

HFONT makeFont(int points, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g.dpi), 72), 0, 0, 0,
                       weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

void applyFonts() {
    deleteFonts();
    g.labelFont = makeFont(9, FW_SEMIBOLD, L"Segoe UI Variable Text");
    g.bodyFont = makeFont(11, FW_NORMAL, L"Segoe UI Variable Text");
    g.monoFont = makeFont(11, FW_NORMAL, L"Cascadia Mono");
    g.buttonFont = makeFont(10, FW_SEMIBOLD, L"Segoe UI Variable Text");

    for (HWND control : {g.inputLabel, g.modeLabel, g.nightlordLabel, g.configLabel,
                         g.variantLabel, g.status}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.labelFont), TRUE);
    }
    for (HWND control : {g.mode, g.nightlord, g.nightlordValue, g.variant}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.bodyFont), TRUE);
    }
    for (HWND control : {g.input, g.result, g.configPath}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.monoFont), TRUE);
    }
    for (HWND control : {g.tabPattern, g.tabSeed, g.action, g.copy, g.browse, g.writeConfig}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.buttonFont), TRUE);
    }
}

HWND createStatic(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                           0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND createButton(HWND parent, int id, const wchar_t* text) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

HWND createCombo(HWND parent, int id) {
    return CreateWindowExW(0, L"COMBOBOX", nullptr,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                               CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                           0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

std::wstring windowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length) GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void setWindowTextQuiet(HWND control, const std::wstring& text) {
    g.suppressChanges = true;
    SetWindowTextW(control, text.c_str());
    g.suppressChanges = false;
}

void setStatus(const std::wstring& text) {
    SetWindowTextW(g.status, text.c_str());
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

bool parsePattern(const std::wstring& text, int& value) {
    std::uint32_t parsed = 0;
    if (!parseUnsigned32(text, parsed) || parsed > 1199u) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool deepOfNight() {
    return ComboBox_GetCurSel(g.mode) == 0;
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

int randomPattern() {
    const std::uint32_t index = randomUint32() % 520u;
    return index < 320u ? static_cast<int>(index)
                        : 1000 + static_cast<int>(index - 320u);
}

void clearResult() {
    g.activeSeed.reset();
    g.activeNightlord = -1;
    EnableWindow(g.copy, FALSE);
    EnableWindow(g.writeConfig, FALSE);
    SetWindowTextW(g.result, L"");
}

void updatePatternOwner() {
    if (g.view != View::PatternToSeed) return;
    int pattern = 0;
    const auto text = windowText(g.input);
    if (!parsePattern(text, pattern)) {
        SetWindowTextW(g.nightlordValue, L"—");
        return;
    }
    const auto owner = derandomizer::nightlordOf(pattern);
    if (!owner) {
        SetWindowTextW(g.nightlordValue, L"Unassigned");
        return;
    }
    const std::wstring value = L"nl" + std::to_wstring(*owner) + L"  " +
                               derandomizer::nightlordName(*owner);
    SetWindowTextW(g.nightlordValue, value.c_str());
}

void setView(View view) {
    if (g.view == View::PatternToSeed) g.patternText = windowText(g.input);
    else g.seedText = windowText(g.input);
    g.view = view;

    const bool reverse = view == View::PatternToSeed;
    SetWindowTextW(g.inputLabel, reverse ? L"Pattern" : L"Seed");
    setWindowTextQuiet(g.input, reverse ? g.patternText : g.seedText);
    SetWindowTextW(g.action, reverse ? L"Find Seed" : L"Get Pattern");
    SetWindowTextW(g.nightlordLabel, L"Nightlord");
    ShowWindow(g.nightlordValue, reverse ? SW_SHOW : SW_HIDE);
    ShowWindow(g.nightlord, reverse ? SW_HIDE : SW_SHOW);
    ShowWindow(g.copy, reverse ? SW_SHOW : SW_HIDE);
    updatePatternOwner();
    clearResult();

    // The parent uses WS_CLIPCHILDREN, so invalidating only the parent does not
    // repaint owner-drawn tab buttons. Reset both transient pressed states and
    // redraw both tabs whenever the logical view changes.
    SendMessageW(g.tabPattern, BM_SETSTATE, FALSE, 0);
    SendMessageW(g.tabSeed, BM_SETSTATE, FALSE, 0);
    RedrawWindow(g.tabPattern, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    RedrawWindow(g.tabSeed, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    InvalidateRect(g.window, nullptr, TRUE);
}

void layout(int width, int) {
    const int margin = scale(32);
    const int inner = scale(24);
    const int cardX = margin;
    const int cardW = width - margin * 2;
    const int right = cardX + cardW - inner;

    MoveWindow(g.tabPattern, margin, scale(24), scale(180), scale(38), TRUE);
    MoveWindow(g.tabSeed, margin + scale(188), scale(24), scale(180), scale(38), TRUE);

    MoveWindow(g.inputLabel, cardX + inner, scale(96), scale(220), scale(20), TRUE);
    MoveWindow(g.input, cardX + inner, scale(120), cardW - inner * 2, scale(40), TRUE);
    MoveWindow(g.modeLabel, cardX + inner, scale(174), scale(220), scale(20), TRUE);
    MoveWindow(g.mode, cardX + inner, scale(198), (cardW - inner * 3) / 2, scale(180), TRUE);
    const int splitX = cardX + inner * 2 + (cardW - inner * 3) / 2;
    MoveWindow(g.nightlordLabel, splitX, scale(174), right - splitX, scale(20), TRUE);
    MoveWindow(g.nightlord, splitX, scale(198), right - splitX, scale(180), TRUE);
    MoveWindow(g.nightlordValue, splitX, scale(198), right - splitX, scale(40), TRUE);

    MoveWindow(g.action, cardX + inner, scale(264), scale(160), scale(42), TRUE);
    MoveWindow(g.copy, cardX + inner + scale(168), scale(264), scale(96), scale(42), TRUE);
    MoveWindow(g.result, cardX + inner + scale(272), scale(264),
               right - (cardX + inner + scale(272)), scale(42), TRUE);

    const int variantWidth = scale(138);
    const int browseWidth = scale(84);
    const int pathRight = right - variantWidth - inner - browseWidth - scale(8);
    MoveWindow(g.configLabel, cardX + inner, scale(358), scale(260), scale(20), TRUE);
    MoveWindow(g.configPath, cardX + inner, scale(382), pathRight - (cardX + inner), scale(40), TRUE);
    MoveWindow(g.browse, pathRight + scale(8), scale(382), browseWidth, scale(40), TRUE);
    MoveWindow(g.variantLabel, right - variantWidth, scale(358), variantWidth, scale(20), TRUE);
    MoveWindow(g.variant, right - variantWidth, scale(382), variantWidth, scale(180), TRUE);
    MoveWindow(g.writeConfig, cardX + inner, scale(438), scale(160), scale(42), TRUE);
    MoveWindow(g.status, cardX + inner + scale(176), scale(448),
               right - (cardX + inner + scale(176)), scale(22), TRUE);

    for (HWND edit : {g.input, g.result, g.configPath}) {
        RECT bounds{};
        GetClientRect(edit, &bounds);
        HRGN region = CreateRoundRectRgn(0, 0, bounds.right + 1, bounds.bottom + 1,
                                         scale(8), scale(8));
        if (!SetWindowRgn(edit, region, TRUE)) DeleteObject(region);
    }
}

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::wstring defaultConfigPath() {
    return L"derandomizer.ini";
}

std::filesystem::path resolveConfigPath(const std::filesystem::path& path) {
    return path.is_relative() ? executableDirectory() / path : path;
}

void browseConfig() {
    std::array<wchar_t, 32768> path{};
    wcsncpy_s(path.data(), path.size(), windowText(g.configPath).c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g.window;
    dialog.lpstrFilter = L"INI files (*.ini)\0*.ini\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrDefExt = L"ini";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog)) SetWindowTextW(g.configPath, path.data());
}

void writeConfig() {
    if (!g.activeSeed || g.activeNightlord < 0) return;
    const std::filesystem::path configuredPath(windowText(g.configPath));
    if (configuredPath.empty()) {
        setStatus(L"Choose an INI file");
        return;
    }
    const std::filesystem::path destination = resolveConfigPath(configuredPath);

    const int variantSelection = ComboBox_GetCurSel(g.variant);
    const int variant = variantSelection == 1 ? 0 : (variantSelection == 2 ? 1 : -1);
    std::wstring error;
    if (!derandomizer::writeModConfig(destination, *g.activeSeed,
                                      g.activeNightlord, variant, error)) {
        setStatus(error);
        return;
    }
    setStatus(L"INI written");
}

void copySeed() {
    if (!g.activeSeed || !OpenClipboard(g.window)) return;
    EmptyClipboard();
    const std::wstring text = hex32(*g.activeSeed);
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        memcpy(target, text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    }
    CloseClipboard();
    setStatus(L"Seed copied");
}

std::pair<std::uint32_t, std::uint32_t> randomPath() {
    return {randomUint32(), randomUint32() | 1u};
}

void calculate() {
    if (g.view == View::PatternToSeed) {
        int pattern = 0;
        if (!parsePattern(windowText(g.input), pattern)) {
            setStatus(L"Pattern must be an integer from 0 to 1199");
            return;
        }
        const auto owner = derandomizer::nightlordOf(pattern);
        if (!owner) {
            setStatus(L"Pattern is unassigned");
            return;
        }
        if (!derandomizer::isReachable(pattern, deepOfNight())) {
            setStatus(L"Pattern is unavailable in this mode");
            return;
        }

        setStatus(L"Searching…");
        UpdateWindow(g.window);
        const auto [start, stride] = randomPath();
        const auto found =
            derandomizer::findSeedRandom(pattern, deepOfNight(), start, stride);
        if (!found) {
            setStatus(L"No seed found");
            return;
        }
        g.activeSeed = found->seed;
        g.activeNightlord = *owner;
        EnableWindow(g.copy, TRUE);
        EnableWindow(g.writeConfig, TRUE);

        std::wostringstream output;
        output << L"Seed  " << hex32(found->seed);
        SetWindowTextW(g.result, output.str().c_str());
        setStatus(L"Seed found");
        return;
    }

    std::uint32_t seed = 0;
    if (!parseUnsigned32(windowText(g.input), seed)) {
        setStatus(L"Seed must be from 0 to 0xFFFFFFFF");
        return;
    }
    const int nightlord = ComboBox_GetCurSel(g.nightlord);
    if (nightlord < 0 || nightlord > 9) {
        setStatus(L"Choose a Nightlord");
        return;
    }
    const auto prediction = derandomizer::predict(nightlord, deepOfNight(), seed);
    if (!prediction) {
        setStatus(L"Calculation failed");
        return;
    }
    g.activeSeed = seed;
    g.activeNightlord = nightlord;
    EnableWindow(g.writeConfig, TRUE);

    std::wostringstream output;
    output << L"Pattern  " << prediction->pattern;
    SetWindowTextW(g.result, output.str().c_str());
    setStatus(L"Pattern calculated");
}

void fillRoundedRect(HDC dc, const RECT& rect, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void paintWindow(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, g.backgroundBrush);

    RECT mainCard{scale(32), scale(80), client.right - scale(32), scale(330)};
    RECT configCard{scale(32), scale(342), client.right - scale(32), scale(498)};
    fillRoundedRect(dc, mainCard, kCard, scale(14));
    fillRoundedRect(dc, configCard, kCard, scale(14));

    EndPaint(window, &paint);
}

void drawButton(const DRAWITEMSTRUCT* item) {
    wchar_t text[128]{};
    GetWindowTextW(item->hwndItem, text, static_cast<int>(std::size(text)));
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool isTab = item->CtlID == ID_TAB_PATTERN || item->CtlID == ID_TAB_SEED;
    const bool activeTab = (item->CtlID == ID_TAB_PATTERN && g.view == View::PatternToSeed) ||
                           (item->CtlID == ID_TAB_SEED && g.view == View::SeedToPattern);
    const bool primary = item->CtlID == ID_ACTION || item->CtlID == ID_WRITE_CONFIG;

    // Owner-drawn buttons still receive a rectangular DC. Clear the pixels
    // outside our rounded shape with the surface underneath the control first.
    HBRUSH surface = CreateSolidBrush(isTab ? kBackground : kCard);
    FillRect(item->hDC, &item->rcItem, surface);
    DeleteObject(surface);

    COLORREF fill = kCardRaised;
    COLORREF color = disabled ? RGB(91, 98, 109) : kText;
    if (isTab && activeTab) fill = kAccentDark;
    if (primary && !disabled) {
        fill = pressed ? RGB(75, 184, 169) : kAccent;
        color = RGB(8, 29, 27);
    } else if (pressed && !disabled) {
        fill = RGB(39, 47, 59);
    }
    fillRoundedRect(item->hDC, item->rcItem, fill, scale(9));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, color);
    SelectObject(item->hDC, g.buttonFont);
    RECT textRect = item->rcItem;
    DrawTextW(item->hDC, text, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (isTab && activeTab) {
        HBRUSH brush = CreateSolidBrush(kAccent);
        RECT line{item->rcItem.left + scale(18), item->rcItem.bottom - scale(3),
                  item->rcItem.right - scale(18), item->rcItem.bottom};
        FillRect(item->hDC, &line, brush);
        DeleteObject(brush);
    }
}

void drawCombo(const DRAWITEMSTRUCT* item) {
    if (item->itemID == static_cast<UINT>(-1)) return;
    wchar_t text[256]{};
    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    HBRUSH brush = CreateSolidBrush(selected ? kAccentDark : kCardRaised);
    FillRect(item->hDC, &item->rcItem, brush);
    DeleteObject(brush);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, selected ? kText : kMuted);
    SelectObject(item->hDC, g.bodyFont);
    RECT rect = item->rcItem;
    rect.left += scale(10);
    DrawTextW(item->hDC, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void drawComboArrow(HWND combo, HDC dc) {
    RECT client{};
    GetClientRect(combo, &client);
    const int inset = scale(1);
    const int arrowWidth = scale(30);
    RECT arrow{client.right - arrowWidth, inset, client.right - inset, client.bottom - inset};
    HBRUSH background = CreateSolidBrush(kCardRaised);
    FillRect(dc, &arrow, background);
    DeleteObject(background);

    HPEN separator = CreatePen(PS_SOLID, 1, kBorder);
    HPEN chevron = CreatePen(PS_SOLID, scale(2), IsWindowEnabled(combo) ? kMuted : RGB(83, 91, 104));
    HGDIOBJ oldPen = SelectObject(dc, separator);
    MoveToEx(dc, arrow.left, arrow.top + scale(7), nullptr);
    LineTo(dc, arrow.left, arrow.bottom - scale(7));
    SelectObject(dc, chevron);
    const int centerX = (arrow.left + arrow.right) / 2;
    const int centerY = (arrow.top + arrow.bottom) / 2;
    MoveToEx(dc, centerX - scale(4), centerY - scale(2), nullptr);
    LineTo(dc, centerX, centerY + scale(2));
    LineTo(dc, centerX + scale(4), centerY - scale(2));
    SelectObject(dc, oldPen);
    DeleteObject(separator);
    DeleteObject(chevron);
}

LRESULT CALLBACK comboProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const LRESULT result = CallWindowProcW(g.comboWindowProc, window, message, wParam, lParam);
    if (message == WM_PAINT) {
        HDC dc = GetDC(window);
        drawComboArrow(window, dc);
        ReleaseDC(window, dc);
    } else if (message == WM_PRINTCLIENT) {
        drawComboArrow(window, reinterpret_cast<HDC>(wParam));
    } else if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, TRUE);
    }
    return result;
}

void initializeControls(HWND window) {
    g.tabPattern = createButton(window, ID_TAB_PATTERN, L"PATTERN → SEED");
    g.tabSeed = createButton(window, ID_TAB_SEED, L"SEED → PATTERN");
    g.inputLabel = createStatic(window, ID_INPUT_LABEL, L"");
    g.input = CreateWindowExW(0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_INPUT)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g.input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scale(12), scale(12)));

    g.modeLabel = createStatic(window, ID_MODE_LABEL, L"Mode");
    g.mode = createCombo(window, ID_MODE);
    ComboBox_AddString(g.mode, L"Deep of Night");
    ComboBox_AddString(g.mode, L"Normal");
    ComboBox_SetCurSel(g.mode, 0);

    g.nightlordLabel = createStatic(window, ID_NIGHTLORD_LABEL, L"");
    g.nightlord = createCombo(window, ID_NIGHTLORD);
    for (int i = 0; i < 10; ++i) {
        const std::wstring item = L"nl" + std::to_wstring(i) + L"  " +
                                  derandomizer::nightlordName(i);
        ComboBox_AddString(g.nightlord, item.c_str());
    }
    ComboBox_SetCurSel(g.nightlord, 0);
    g.nightlordValue = createStatic(window, ID_NIGHTLORD_VALUE, L"");

    g.action = createButton(window, ID_ACTION, L"");
    g.copy = createButton(window, ID_COPY, L"Copy");
    g.result = CreateWindowExW(0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESULT)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g.result, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scale(12), scale(12)));
    g.configLabel = createStatic(window, ID_CONFIG_LABEL, L"INI File");
    g.configPath = CreateWindowExW(0, L"EDIT", defaultConfigPath().c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CONFIG_PATH)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(g.configPath, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(scale(10), scale(10)));
    g.browse = createButton(window, ID_BROWSE, L"Browse");
    g.variantLabel = createStatic(window, ID_VARIANT_LABEL, L"Variant");
    g.variant = createCombo(window, ID_VARIANT);
    ComboBox_AddString(g.variant, L"Any (-1)");
    ComboBox_AddString(g.variant, L"Normal (0)");
    ComboBox_AddString(g.variant, L"Everdark (1)");
    ComboBox_SetCurSel(g.variant, 0);
    g.writeConfig = createButton(window, ID_WRITE_CONFIG, L"Write INI");
    g.status = createStatic(window, ID_STATUS, L"Ready");

    for (HWND edit : {g.input, g.result, g.configPath}) SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);
    for (HWND combo : {g.mode, g.nightlord, g.variant}) SetWindowTheme(combo, L"DarkMode_CFD", nullptr);
    g.comboWindowProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g.mode, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(comboProc)));
    SetWindowLongPtrW(g.nightlord, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(comboProc));
    SetWindowLongPtrW(g.variant, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(comboProc));
    applyFonts();
    EnableWindow(g.copy, FALSE);
    EnableWindow(g.writeConfig, FALSE);
    g.patternText = std::to_wstring(randomPattern());
    g.seedText = hex32(randomUint32());
    setWindowTextQuiet(g.input, g.patternText);
    setView(View::PatternToSeed);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g.window = window;
        g.dpi = GetDpiForWindow(window);
        g.backgroundBrush = CreateSolidBrush(kBackground);
        g.cardBrush = CreateSolidBrush(kCard);
        g.editBrush = CreateSolidBrush(kCardRaised);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        initializeControls(window);
        return 0;
    }
    case WM_SIZE:
        layout(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        applyFonts();
        RECT client{};
        GetClientRect(window, &client);
        layout(client.right, client.bottom);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = scale(680);
        info->ptMinTrackSize.y = scale(558);
        return 0;
    }
    case WM_PAINT:
        paintWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND control = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(control);
        const bool onRaised = id == ID_RESULT || id == ID_NIGHTLORD_VALUE;
        const COLORREF background = onRaised ? kCardRaised : kCard;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, background);
        SetTextColor(dc, (id == ID_NIGHTLORD_VALUE || id == ID_RESULT)
                             ? kText
                             : (id == ID_STATUS ? kAccent : kMuted));
        return reinterpret_cast<LRESULT>(onRaised ? g.editBrush : g.cardBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kCardRaised);
        SetTextColor(dc, kText);
        return reinterpret_cast<LRESULT>(g.editBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kCardRaised);
        SetTextColor(dc, kText);
        return reinterpret_cast<LRESULT>(g.editBrush);
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlType == ODT_COMBOBOX) {
            measure->itemHeight = scale(34);
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) drawButton(item);
        else if (item->CtlType == ODT_COMBOBOX) drawCombo(item);
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);
        if (notification == BN_CLICKED) {
            if (id == ID_TAB_PATTERN) setView(View::PatternToSeed);
            else if (id == ID_TAB_SEED) setView(View::SeedToPattern);
            else if (id == ID_ACTION) calculate();
            else if (id == ID_COPY) copySeed();
            else if (id == ID_BROWSE) browseConfig();
            else if (id == ID_WRITE_CONFIG) writeConfig();
            return 0;
        }
        if (!g.suppressChanges && id == ID_INPUT && notification == EN_CHANGE) {
            clearResult();
            updatePatternOwner();
            return 0;
        }
        if ((id == ID_MODE || id == ID_NIGHTLORD) && notification == CBN_SELCHANGE) {
            clearResult();
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        deleteFonts();
        if (g.backgroundBrush) DeleteObject(g.backgroundBrush);
        if (g.cardBrush) DeleteObject(g.cardBrush);
        if (g.editBrush) DeleteObject(g.editBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int runGui(HINSTANCE instance, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = kWindowClass;
    windowClass.hbrBackground = CreateSolidBrush(kBackground);
    if (!RegisterClassExW(&windowClass)) return 1;

    RECT desired{0, 0, 760, 528};
    AdjustWindowRectExForDpi(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0, 96);
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(0, kWindowClass, L"Seed Reroller",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1) return seed_reroller::runCli(argc, argv);

    // Explorer creates a console for a console-subsystem executable. Detach it
    // when this is the only attached process so normal double-click use remains
    // a GUI-only experience. An existing terminal stays attached and waits for
    // the window to close, which is the expected command-line behaviour.
    DWORD processIds[2]{};
    if (GetConsoleProcessList(processIds, static_cast<DWORD>(std::size(processIds))) == 1) {
        FreeConsole();
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    const int showCommand = (startup.dwFlags & STARTF_USESHOWWINDOW)
                                ? startup.wShowWindow
                                : SW_SHOWNORMAL;
    return runGui(GetModuleHandleW(nullptr), showCommand);
}
