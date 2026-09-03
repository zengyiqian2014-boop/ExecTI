// ============================================================================
//  execti-gui - a "Run" dialog that launches programs with high privileges
// ----------------------------------------------------------------------------
//  Like the Windows Run box, but with a full option set (a superset of ExecTI,
//  in the spirit of NSudo):
//    * Token source: TrustedInstaller / SYSTEM / Current user (elevated)
//    * Integrity level: System / High / Medium / Low
//    * Enable all privileges, new-console toggles
//    * editable command box with a dropdown of recently-used entries (MRU)
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
#include <string>
#include <vector>

#include "trustedinstaller.h"

enum {
    ID_PROMPT = 1001, ID_LABEL, ID_COMBO, ID_RUN, ID_CANCEL, ID_BROWSE,
    ID_FOLDER, ID_ICON, ID_SRCLBL, ID_SRC, ID_INTEGLBL, ID_INTEG,
    ID_ALLPRIV, ID_NEWCON
};

static const wchar_t* kRegKey   = L"Software\\ExecTI";
static const wchar_t* kRegValue = L"MRU";
static const int      kMaxMru   = 20;

static HFONT g_font  = nullptr;
static HWND  g_combo = nullptr, g_src = nullptr, g_integ = nullptr,
             g_allpriv = nullptr, g_newcon = nullptr;

// ---- MRU history (registry) ------------------------------------------------
static std::vector<std::wstring> LoadMru() {
    std::vector<std::wstring> out;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return out;
    for (int i = 0; i < kMaxMru; ++i) {
        wchar_t name[32];
        _snwprintf(name, 32, L"%s%d", kRegValue, i);
        wchar_t data[1024];
        DWORD cb = sizeof(data), type = 0;
        if (RegQueryValueExW(hk, name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(data), &cb) == ERROR_SUCCESS &&
            type == REG_SZ && data[0])
            out.emplace_back(data);
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
        wchar_t name[32];
        _snwprintf(name, 32, L"%s%d", kRegValue, i);
        if (i < static_cast<int>(items.size()))
            RegSetValueExW(hk, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(items[i].c_str()),
                           static_cast<DWORD>((items[i].size() + 1) * sizeof(wchar_t)));
        else
            RegDeleteValueW(hk, name);
    }
    RegCloseKey(hk);
}

static void PushMru(const std::wstring& entry) {
    if (entry.empty()) return;
    std::vector<std::wstring> items = LoadMru();
    for (auto it = items.begin(); it != items.end();) {
        if (_wcsicmp(it->c_str(), entry.c_str()) == 0) it = items.erase(it);
        else ++it;
    }
    items.insert(items.begin(), entry);
    if (static_cast<int>(items.size()) > kMaxMru) items.resize(kMaxMru);
    SaveMru(items);
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& s : items)
        SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
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
    wchar_t expanded[2048];
    cmdline = ExpandEnvironmentStringsW(raw.c_str(), expanded, 2048) ? expanded : raw;
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
        DestroyWindow(hwnd);
    } else {
        MessageBoxW(hwnd, err.c_str(), L"ExecTI - Launch failed", MB_ICONERROR);
    }
}

// ---- window ----------------------------------------------------------------
static HWND MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text,
                        DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    if (g_font) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return c;
}
static void AddItem(HWND combo, const wchar_t* s) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HWND ico = MakeControl(hwnd, L"STATIC", L"", SS_ICON, 14, 16, 32, 32, ID_ICON);
        HICON hIcon = LoadIconW(nullptr, IDI_SHIELD);
        if (hIcon) SendMessageW(ico, STM_SETICON, reinterpret_cast<WPARAM>(hIcon), 0);

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
        MakeControl(hwnd, L"BUTTON", L"Cancel", WS_TABSTOP, 180, 170, 90, 28, ID_CANCEL);
        MakeControl(hwnd, L"BUTTON", L"&Browse...", WS_TABSTOP, 278, 170, 90, 28, ID_BROWSE);
        MakeControl(hwnd, L"BUTTON", L"&Folder...", WS_TABSTOP, 376, 170, 90, 28, ID_FOLDER);

        for (const auto& s : LoadMru()) AddItem(g_combo, s.c_str());
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
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);

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

    const int cw = 482, ch = 214;
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"ExecTI - run with high privileges",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, sx, sy, ww, wh,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
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
    CoUninitialize();
    return 0;
}
