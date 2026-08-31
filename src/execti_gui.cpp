// ============================================================================
//  execti-gui - a "Run" dialog that launches programs as TrustedInstaller
// ----------------------------------------------------------------------------
//  Works like the Windows Run box (Win+R) but launches the target with
//  TrustedInstaller privileges. Extras over the stock Run dialog:
//    * editable combo box with a dropdown arrow showing recently used entries
//      (MRU history persisted in HKCU\Software\ExecTI)
//    * a "Browse..." button to pick a program file
//    * a "Folder..." button to pick a folder (open it / use as working dir)
//
//  Native Win32, no external dependencies. Compiled with MinGW for x86_64 and
//  ARM64. Must be run elevated (the embedded manifest requests Administrator).
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

// ---- control ids -----------------------------------------------------------
enum {
    ID_PROMPT = 1001,
    ID_LABEL,
    ID_COMBO,
    ID_RUN,
    ID_CANCEL,
    ID_BROWSE,
    ID_FOLDER,
    ID_ICON
};

static const wchar_t* kRegKey    = L"Software\\ExecTI";
static const wchar_t* kRegValue  = L"MRU";           // value prefix: MRU0, MRU1...
static const int      kMaxMru    = 20;

static HFONT   g_font   = nullptr;
static HWND    g_combo  = nullptr;

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
            type == REG_SZ && data[0]) {
            out.emplace_back(data);
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
        wchar_t name[32];
        _snwprintf(name, 32, L"%s%d", kRegValue, i);
        if (i < static_cast<int>(items.size())) {
            RegSetValueExW(hk, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(items[i].c_str()),
                           static_cast<DWORD>((items[i].size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hk, name);
        }
    }
    RegCloseKey(hk);
}

// Move `entry` to the front of the MRU list, dedupe, cap, persist, refill combo.
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
    // trim
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
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash);
}

// Expand %VARS%, and if a bare file/dir path, quote/normalize into a command.
static void BuildCommand(const std::wstring& raw, std::wstring& cmdline,
                         std::wstring& workDir) {
    // Expand environment variables like the Run dialog does.
    wchar_t expanded[2048];
    if (ExpandEnvironmentStringsW(raw.c_str(), expanded, 2048))
        cmdline = expanded;
    else
        cmdline = raw;

    // Strip surrounding quotes to test the path.
    std::wstring probe = cmdline;
    if (probe.size() >= 2 && probe.front() == L'"' && probe.back() == L'"')
        probe = probe.substr(1, probe.size() - 2);

    if (IsDirectory(probe)) {
        // Open the folder in Explorer, running as TrustedInstaller.
        cmdline = L"explorer.exe \"" + probe + L"\"";
        workDir = probe;
    } else if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Existing file: run it, working dir = its folder.
        cmdline = L"\"" + probe + L"\"";
        workDir = ParentDir(probe);
    } else {
        // Bare command (e.g. "regedit", "cmd /c ..."). Pass through as typed.
        workDir.clear();
    }
}

// ---- browse dialogs --------------------------------------------------------
static void BrowseFile(HWND hwnd) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"程序 (*.exe;*.bat;*.cmd;*.msc)\0*.exe;*.bat;*.cmd;*.msc\0"
                      L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = 1024;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn))
        SetWindowTextW(g_combo, file);
}

static void BrowseFolder(HWND hwnd) {
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"选择一个文件夹";  // "Select a folder"
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path))
            SetWindowTextW(g_combo, path);
        CoTaskMemFree(pidl);
    }
}

// ---- run -------------------------------------------------------------------
static void DoRun(HWND hwnd) {
    std::wstring raw = ComboText();
    if (raw.empty()) {
        MessageBoxW(hwnd, L"请输入要运行的程序、"
                          L"文件或文件夹。",  // please enter...
                    L"ExecTI", MB_ICONINFORMATION);
        return;
    }
    std::wstring cmdline, workDir;
    BuildCommand(raw, cmdline, workDir);

    DWORD pid = 0;
    std::wstring err;
    if (ti::RunAsTI(cmdline, workDir, pid, err)) {
        PushMru(raw);  // remember exactly what the user typed
        DestroyWindow(hwnd);  // close on success, like the Run dialog
    } else {
        MessageBoxW(hwnd, err.c_str(),
                    L"ExecTI - 启动失败",  // "launch failed"
                    MB_ICONERROR);
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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // shield icon (UAC)
        HWND ico = MakeControl(hwnd, L"STATIC", L"", SS_ICON,
                               14, 16, 32, 32, ID_ICON);
        HICON hIcon = LoadIconW(nullptr, IDI_SHIELD);
        if (hIcon) SendMessageW(ico, STM_SETICON, reinterpret_cast<WPARAM>(hIcon), 0);

        MakeControl(hwnd, L"STATIC",
                    L"输入程序、文件夹或文件"
                    L"名，ExecTI 将以 TrustedInstaller "
                    L"权限运行它。",  // "Type a program/folder/file; ExecTI will run it as TrustedInstaller."
                    0, 56, 14, 392, 44, ID_PROMPT);

        MakeControl(hwnd, L"STATIC", L"打开(&O):",  // "Open:"
                    0, 62, 12, 44, 20, ID_LABEL);

        // editable combo box with dropdown history (the "little arrow")
        g_combo = MakeControl(hwnd, L"COMBOBOX", L"",
                              CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL |
                                  WS_TABSTOP,
                              56, 58, 392, 220, ID_COMBO);

        // buttons row
        MakeControl(hwnd, L"BUTTON", L"运行(&R)",  // "Run"
                    BS_DEFPUSHBUTTON | WS_TABSTOP, 64, 100, 90, 28, ID_RUN);
        MakeControl(hwnd, L"BUTTON", L"取消",  // "Cancel"
                    WS_TABSTOP, 162, 100, 90, 28, ID_CANCEL);
        MakeControl(hwnd, L"BUTTON", L"浏览(&B)...",  // "Browse..."
                    WS_TABSTOP, 260, 100, 90, 28, ID_BROWSE);
        MakeControl(hwnd, L"BUTTON", L"文件夹(&F)...",  // "Folder..."
                    WS_TABSTOP, 358, 100, 90, 28, ID_FOLDER);

        // fill history + focus the combo edit
        for (const auto& s : LoadMru())
            SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
        SetFocus(g_combo);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_RUN)      { DoRun(hwnd); return 0; }
        if (id == ID_CANCEL)   { DestroyWindow(hwnd); return 0; }
        if (id == ID_BROWSE)   { BrowseFile(hwnd); return 0; }
        if (id == ID_FOLDER)   { BrowseFolder(hwnd); return 0; }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for SHBrowseForFolder
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Use the standard message-box font (looks native, renders CJK correctly).
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
    wc.hIcon         = LoadIconW(nullptr, IDI_SHIELD);
    RegisterClassExW(&wc);

    // Fixed-size dialog-like window, centered.
    const int cw = 462, ch = 150;
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"ExecTI - 以 TrustedInstaller 运行",  // "Run as TrustedInstaller"
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        sx, sy, ww, wh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message loop with dialog navigation (Tab / Enter / Esc between controls).
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
