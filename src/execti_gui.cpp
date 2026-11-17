// ============================================================================
//  execti-gui - a "Run" dialog that launches programs with high privileges
// ----------------------------------------------------------------------------
//  Like the Windows Run box, but with a full option set (a superset of ExecTI,
//  in the spirit of NSudo):
//    * Token source: TrustedInstaller / SYSTEM / Current user (elevated)
//    * Integrity level: System / High / Medium / Low
//    * Enable all privileges, new-console toggles
//    * editable command box with recent history and built-in program suggestions
//    * Browse (file) and Folder pickers
//
//  Native Win32, no external dependencies. Cross-compiled for x86_64 and ARM64.
//  Must be run elevated (the embedded manifest requests Administrator).
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <cstring>
#include <string>
#include <vector>

#include "trustedinstaller.h"
#include "command_history.h"

enum {
    ID_PROMPT = 1001, ID_LABEL, ID_COMBO, ID_RUN, ID_CANCEL, ID_BROWSE,
    ID_FOLDER, ID_ICON, ID_SRCLBL, ID_SRC, ID_INTEGLBL, ID_INTEG,
    ID_ALLPRIV, ID_NEWCON
};

static const wchar_t* kRegKey   = L"Software\\ExecTI";
static const wchar_t* kRegValue = L"MRU";
static const int      kMaxMru   = static_cast<int>(command_history::kMaxHistory);

static const UINT kBaseDpi = 96;
static const int kClientWidth = 482, kClientHeight = 214;
static UINT  g_dpi   = kBaseDpi;
static HFONT g_font  = nullptr;
static HICON g_promptIcon = nullptr;
static HWND  g_combo = nullptr, g_src = nullptr, g_integ = nullptr,
             g_allpriv = nullptr, g_newcon = nullptr;

// Keep all layout measurements in 96-DPI units. Resolve newer APIs at runtime
// so the same binaries still start on the older Windows versions we support.
static int Scale(int value) {
    return MulDiv(value, static_cast<int>(g_dpi), kBaseDpi);
}

template<typename Function>
static Function User32Function(const char* name) {
    FARPROC address = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address), "Windows function pointer size");
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

static UINT SystemDpi() {
    using GetDpiFn = UINT (WINAPI*)();
    static auto getDpi = User32Function<GetDpiFn>("GetDpiForSystem");
    if (getDpi) {
        UINT dpi = getDpi();
        if (dpi) return dpi;
    }
    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : kBaseDpi;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : kBaseDpi;
}

static UINT WindowDpi(HWND hwnd) {
    using GetDpiFn = UINT (WINAPI*)(HWND);
    static auto getDpi = User32Function<GetDpiFn>("GetDpiForWindow");
    UINT dpi = getDpi ? getDpi(hwnd) : 0;
    return dpi ? dpi : SystemDpi();
}

static void AdjustWindowForDpi(RECT& rect, DWORD style, DWORD exStyle) {
    using AdjustFn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static auto adjust = User32Function<AdjustFn>("AdjustWindowRectExForDpi");
    if (!adjust || !adjust(&rect, style, FALSE, exStyle, g_dpi))
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
}

static BOOL CALLBACK SetControlFont(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    return TRUE;
}

static void UpdateDpiFont(HWND hwnd) {
    using MetricsFn = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    static auto metrics = User32Function<MetricsFn>("SystemParametersInfoForDpi");
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!metrics || !metrics(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g_dpi)) {
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return;
        UINT sourceDpi = SystemDpi();
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, g_dpi, sourceDpi);
        ncm.lfMessageFont.lfWidth = MulDiv(ncm.lfMessageFont.lfWidth, g_dpi, sourceDpi);
    }
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!font) return;
    HFONT oldFont = g_font;
    g_font = font;
    EnumChildWindows(hwnd, SetControlFont, reinterpret_cast<LPARAM>(font));
    if (oldFont) DeleteObject(oldFont);
}

static void UpdatePromptIcon(HWND hwnd) {
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(1), IMAGE_ICON, Scale(32), Scale(32), 0));
    if (!icon) icon = static_cast<HICON>(LoadImageW(nullptr, IDI_SHIELD,
        IMAGE_ICON, Scale(32), Scale(32), 0));
    if (!icon) return;
    SendDlgItemMessageW(hwnd, ID_ICON, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
    if (g_promptIcon) DestroyIcon(g_promptIcon);
    g_promptIcon = icon;
}

// ---- MRU history (registry) ------------------------------------------------
static std::vector<std::wstring> LoadMru() {
    std::vector<std::wstring> out;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return out;
    for (int i = 0; i < kMaxMru; ++i) {
        const std::wstring name = std::wstring(kRegValue) + std::to_wstring(i);
        DWORD cb = 0, type = 0;
        constexpr DWORD maxBytes = 32768 * sizeof(wchar_t);
        if (RegQueryValueExW(hk, name.c_str(), nullptr, &type, nullptr, &cb) != ERROR_SUCCESS ||
            type != REG_SZ || cb == 0 || cb > maxBytes || cb % sizeof(wchar_t) != 0)
            continue;
        // RegQueryValueEx does not guarantee a terminating NUL. Reserve one
        // ourselves, and reject values that change type/size while being read.
        const DWORD capacity = cb;
        std::vector<wchar_t> data(cb / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(hk, name.c_str(), nullptr, &type,
                reinterpret_cast<LPBYTE>(data.data()), &cb) == ERROR_SUCCESS &&
            type == REG_SZ && cb <= capacity && cb % sizeof(wchar_t) == 0) {
            data[cb / sizeof(wchar_t)] = L'\0';
            const std::wstring entry(data.data());
            if (!entry.empty() && entry.size() <= 32767) out.push_back(entry);
        }
    }
    RegCloseKey(hk);
    return out;
}

static void SaveMru(const std::vector<std::wstring>& items) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    for (int i = 0; i < kMaxMru; ++i) {
        const std::wstring name = std::wstring(kRegValue) + std::to_wstring(i);
        if (i < static_cast<int>(items.size()))
            RegSetValueExW(hk, name.c_str(), 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(items[i].c_str()),
                           static_cast<DWORD>((items[i].size() + 1) * sizeof(wchar_t)));
        else
            RegDeleteValueW(hk, name.c_str());
    }
    RegCloseKey(hk);
}

static void RefreshCommandSuggestions(const std::vector<std::wstring>& history) {
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& s : command_history::BuildSuggestions(history))
        SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
}

static void PushMru(const std::wstring& entry) {
    if (entry.empty()) return;
    const auto items = command_history::BuildHistory(LoadMru(), entry);
    SaveMru(items);
    RefreshCommandSuggestions(items);
}

// ---- helpers ---------------------------------------------------------------
static std::wstring ComboText() {
    int len = GetWindowTextLengthW(g_combo);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(g_combo, &s[0], len + 1);
    s.resize(len);
    size_t a = s.find_first_not_of(L" \t");
    size_t b = s.find_last_not_of(L" \t");
    if (a == std::wstring::npos) return L"";
    return s.substr(a, b - a + 1);
}

static bool IsDirectory(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring ParentDir(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : path.substr(0, s);
}

static void BuildCommand(const std::wstring& raw, std::wstring& cmdline,
                         std::wstring& workDir) {
    cmdline = raw;
    DWORD needed = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
    if (needed && needed <= 32768) {
        std::vector<wchar_t> expanded(needed, L'\0');
        DWORD written = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), needed);
        if (written && written <= needed) cmdline.assign(expanded.data());
    }
    std::wstring probe = cmdline;
    if (probe.size() >= 2 && probe.front() == L'"' && probe.back() == L'"')
        probe = probe.substr(1, probe.size() - 2);
    if (IsDirectory(probe)) {
        cmdline = L"explorer.exe \"" + probe + L"\"";
        workDir = probe;
    } else if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        cmdline = L"\"" + probe + L"\"";
        workDir = ParentDir(probe);
    } else {
        workDir.clear();
    }
}

// ---- browse dialogs --------------------------------------------------------
static void BrowseFile(HWND hwnd) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Programs (*.exe;*.bat;*.cmd;*.msc)\0*.exe;*.bat;*.cmd;*.msc\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = 1024;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_combo, file);
}
static void BrowseFolder(HWND hwnd) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Select a folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_combo, path);
        CoTaskMemFree(pidl);
    }
}

// ---- run -------------------------------------------------------------------
static void DoRun(HWND hwnd) {
    std::wstring raw = ComboText();
    if (raw.empty()) {
        MessageBoxW(hwnd, L"Please enter a program, file, or folder to run.",
                    L"ExecTI", MB_ICONINFORMATION);
        return;
    }
    ti::Options o;
    switch (SendMessageW(g_src, CB_GETCURSEL, 0, 0)) {
        case 1:  o.source = ti::Source::System;      break;
        case 2:  o.source = ti::Source::CurrentUser; break;
        default: o.source = ti::Source::TrustedInstaller; break;
    }
    switch (SendMessageW(g_integ, CB_GETCURSEL, 0, 0)) {
        case 1:  o.integrityRid = SECURITY_MANDATORY_HIGH_RID;   break;
        case 2:  o.integrityRid = SECURITY_MANDATORY_MEDIUM_RID; break;
        case 3:  o.integrityRid = SECURITY_MANDATORY_LOW_RID;    break;
        default: o.integrityRid = SECURITY_MANDATORY_SYSTEM_RID; break;
    }
    o.enableAllPriv = SendMessageW(g_allpriv, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.newConsole    = SendMessageW(g_newcon,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    BuildCommand(raw, o.cmdline, o.workDir);

    DWORD pid = 0;
    std::wstring err;
    if (ti::Run(o, pid, err)) {
        PushMru(raw);
        // Refreshing the history resets the edit box. Keep the last command
        // ready to reuse or replace, and leave the launcher open.
        SetWindowTextW(g_combo, raw.c_str());
        SendMessageW(g_combo, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
    } else {
        MessageBoxW(hwnd, err.c_str(), L"ExecTI - Launch failed", MB_ICONERROR);
    }
}

// ---- window ----------------------------------------------------------------
static HWND MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text,
                        DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             Scale(x), Scale(y), Scale(w), Scale(h), parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    if (g_font) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}
static void AddItem(HWND combo, const wchar_t* s) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
}

static void MoveCtl(HWND parent, int id, int x, int y, int w, int h) {
    MoveWindow(GetDlgItem(parent, id), x, y, w, h, TRUE);
}

// Reflow the controls for the current client size: the prompt and command box
// stretch with the width; the four buttons stay pinned to the bottom-right.
static void LayoutControls(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    const int R = Scale(16);

    MoveCtl(hwnd, ID_ICON,     Scale(14), Scale(16), Scale(32), Scale(32));
    MoveCtl(hwnd, ID_PROMPT,   Scale(56), Scale(12), cw - Scale(56) - R, Scale(40));
    MoveCtl(hwnd, ID_LABEL,    Scale(14), Scale(66), Scale(44), Scale(20));
    MoveCtl(hwnd, ID_COMBO,    Scale(58), Scale(62), cw - Scale(58) - R, Scale(220));
    MoveCtl(hwnd, ID_SRCLBL,   Scale(14), Scale(104), Scale(46), Scale(18));
    MoveCtl(hwnd, ID_SRC,      Scale(64), Scale(100), Scale(168), Scale(200));
    MoveCtl(hwnd, ID_INTEGLBL, Scale(248), Scale(104), Scale(56), Scale(18));
    MoveCtl(hwnd, ID_INTEG,    Scale(308), Scale(100), Scale(158), Scale(200));
    MoveCtl(hwnd, ID_ALLPRIV,  Scale(64), Scale(134), Scale(190), Scale(22));
    MoveCtl(hwnd, ID_NEWCON,   Scale(264), Scale(134), Scale(150), Scale(22));

    const int bw = Scale(90), bg = Scale(8), by = ch - Scale(38);
    int fx = cw - R - bw, bx = fx - (bw + bg), cx = bx - (bw + bg), rx = cx - (bw + bg);
    MoveCtl(hwnd, ID_RUN,    rx, by, bw, Scale(28));
    MoveCtl(hwnd, ID_CANCEL, cx, by, bw, Scale(28));
    MoveCtl(hwnd, ID_BROWSE, bx, by, bw, Scale(28));
    MoveCtl(hwnd, ID_FOLDER, fx, by, bw, Scale(28));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_dpi = WindowDpi(hwnd);
        UpdateDpiFont(hwnd);
        MakeControl(hwnd, L"STATIC", L"", SS_ICON, 14, 16, 32, 32, ID_ICON);
        // Use the app's own gold-shield icon (resource id 1), matching the .ico /
        // taskbar icon; fall back to the system UAC shield only if it can't load.
        UpdatePromptIcon(hwnd);

        MakeControl(hwnd, L"STATIC",
                    L"Type the name of a program, folder, or file, and ExecTI "
                    L"will run it with the chosen privileges.",
                    0, 56, 12, 410, 40, ID_PROMPT);

        MakeControl(hwnd, L"STATIC", L"&Open:", 0, 14, 66, 44, 20, ID_LABEL);
        g_combo = MakeControl(hwnd, L"COMBOBOX", L"",
                              CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                              58, 62, 408, 220, ID_COMBO);

        // options row 1: token source + integrity
        MakeControl(hwnd, L"STATIC", L"Run as:", 0, 14, 104, 46, 18, ID_SRCLBL);
        g_src = MakeControl(hwnd, L"COMBOBOX", L"",
                            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                            64, 100, 168, 200, ID_SRC);
        AddItem(g_src, L"TrustedInstaller");
        AddItem(g_src, L"SYSTEM");
        AddItem(g_src, L"Current user (elevated)");
        SendMessageW(g_src, CB_SETCURSEL, 0, 0);

        MakeControl(hwnd, L"STATIC", L"Integrity:", 0, 248, 104, 56, 18, ID_INTEGLBL);
        g_integ = MakeControl(hwnd, L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                              308, 100, 158, 200, ID_INTEG);
        AddItem(g_integ, L"System");
        AddItem(g_integ, L"High");
        AddItem(g_integ, L"Medium");
        AddItem(g_integ, L"Low");
        SendMessageW(g_integ, CB_SETCURSEL, 0, 0);

        // options row 2: checkboxes
        g_allpriv = MakeControl(hwnd, L"BUTTON", L"Enable all &privileges",
                                BS_AUTOCHECKBOX | WS_TABSTOP, 64, 134, 190, 22, ID_ALLPRIV);
        SendMessageW(g_allpriv, BM_SETCHECK, BST_CHECKED, 0);
        g_newcon = MakeControl(hwnd, L"BUTTON", L"&New console",
                               BS_AUTOCHECKBOX | WS_TABSTOP, 264, 134, 150, 22, ID_NEWCON);
        SendMessageW(g_newcon, BM_SETCHECK, BST_CHECKED, 0);

        // buttons
        MakeControl(hwnd, L"BUTTON", L"&Run", BS_DEFPUSHBUTTON | WS_TABSTOP,
                    82, 170, 90, 28, ID_RUN);
        MakeControl(hwnd, L"BUTTON", L"Close", WS_TABSTOP, 180, 170, 90, 28, ID_CANCEL);
        MakeControl(hwnd, L"BUTTON", L"&Browse...", WS_TABSTOP, 278, 170, 90, 28, ID_BROWSE);
        MakeControl(hwnd, L"BUTTON", L"&Folder...", WS_TABSTOP, 376, 170, 90, 28, ID_FOLDER);

        RefreshCommandSuggestions(LoadMru());
        SetFocus(g_combo);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_RUN)    { DoRun(hwnd);        return 0; }
        if (id == ID_CANCEL) { DestroyWindow(hwnd); return 0; }
        if (id == ID_BROWSE) { BrowseFile(hwnd);   return 0; }
        if (id == ID_FOLDER) { BrowseFolder(hwnd); return 0; }
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) LayoutControls(hwnd);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        UpdateDpiFont(hwnd);
        UpdatePromptIcon(hwnd);
        const auto* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutControls(hwnd);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, Scale(kClientWidth), Scale(kClientHeight)};
        AdjustWindowForDpi(r, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                          static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g_dpi = SystemDpi();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ExecTIWindow";
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_SHIELD);
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);

    RECT r{0, 0, Scale(kClientWidth), Scale(kClientHeight)};
    AdjustWindowForDpi(r, WS_OVERLAPPEDWINDOW, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"ExecTI - run with high privileges",
        WS_OVERLAPPEDWINDOW, sx, sy, ww, wh,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    // WM_CREATE obtains the actual monitor DPI. Recalculate before showing the
    // window in case it differs from the system DPI used for its initial size.
    r = RECT{0, 0, Scale(kClientWidth), Scale(kClientHeight)};
    AdjustWindowForDpi(r, WS_OVERLAPPEDWINDOW, 0);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        ww = r.right - r.left;
        wh = r.bottom - r.top;
        sx = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - ww) / 2;
        sy = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - wh) / 2;
        SetWindowPos(hwnd, nullptr, sx, sy, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_font) DeleteObject(g_font);
    if (g_promptIcon) DestroyIcon(g_promptIcon);
    CoUninitialize();
    return 0;
}
