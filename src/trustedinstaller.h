// ============================================================================
//  trustedinstaller.h - shared TrustedInstaller launch logic (header-only)
// ----------------------------------------------------------------------------
//  Runs a program under the Windows "TrustedInstaller" service account using
//  only public Win32 token APIs. Shared by the console (execti) and GUI
//  (execti-gui) front ends.
//
//  Flow:
//    1. Enable SeDebugPrivilege on our own token (needs Administrator).
//    2. Impersonate winlogon.exe -> our thread runs as SYSTEM.
//    3. Start the TrustedInstaller service, read TrustedInstaller.exe PID.
//    4. Duplicate that process's token as a primary token.
//    5. CreateProcessWithTokenW() with it to spawn the target as TrustedInstaller.
// ============================================================================
#ifndef EXECTI_TRUSTEDINSTALLER_H
#define EXECTI_TRUSTEDINSTALLER_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <string>
#include <vector>

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

namespace ti {

// Format "message (code N)" for a Win32 error.
inline std::wstring FormatErr(const wchar_t* what, DWORD code) {
    wchar_t buf[512];
    _snwprintf(buf, 512, L"%s (error %lu)", what, code);
    return std::wstring(buf);
}

inline bool EnablePrivilege(HANDLE hToken, const wchar_t* name) {
    TOKEN_PRIVILEGES tp{};
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) return false;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr))
        return false;
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

// Enable *every* privilege the token holds (SeDebug, SeTcb, SeLoadDriver, ...).
// The TrustedInstaller token already carries the full high-privilege set; this
// just flips them all to ENABLED so the launched process starts with maximum
// authority ("TrustedInstaller identity + all SYSTEM-grade privileges active").
inline void EnableAllPrivileges(HANDLE hToken) {
    DWORD len = 0;
    GetTokenInformation(hToken, TokenPrivileges, nullptr, 0, &len);
    if (!len) return;
    std::vector<BYTE> buf(len);
    if (!GetTokenInformation(hToken, TokenPrivileges, buf.data(), len, &len))
        return;
    auto* tp = reinterpret_cast<TOKEN_PRIVILEGES*>(buf.data());
    for (DWORD i = 0; i < tp->PrivilegeCount; ++i)
        tp->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, FALSE, tp, len, nullptr, nullptr);
}

inline bool EnableDebugPrivilege(std::wstring& err) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        err = FormatErr(L"OpenProcessToken(self) failed", GetLastError());
        return false;
    }
    bool ok = EnablePrivilege(hToken, SE_DEBUG_NAME);
    CloseHandle(hToken);
    if (!ok) err = L"Could not enable SeDebugPrivilege - run as Administrator.";
    return ok;
}

inline DWORD FindProcessId(const wchar_t* imageName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, imageName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Impersonate SYSTEM by duplicating winlogon.exe's token onto our thread.
inline bool ImpersonateSystem(std::wstring& err) {
    DWORD pid = FindProcessId(L"winlogon.exe");
    if (!pid) { err = L"winlogon.exe not found."; return false; }
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        err = FormatErr(L"OpenProcess(winlogon) failed", GetLastError());
        return false;
    }
    HANDLE hTok = nullptr, hDup = nullptr;
    bool ok = false;
    if (OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
        if (DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                             SecurityImpersonation, TokenImpersonation, &hDup)) {
            if (SetThreadToken(nullptr, hDup)) ok = true;
            else err = FormatErr(L"SetThreadToken failed", GetLastError());
            CloseHandle(hDup);
        } else {
            err = FormatErr(L"DuplicateTokenEx(winlogon) failed", GetLastError());
        }
        CloseHandle(hTok);
    } else {
        err = FormatErr(L"OpenProcessToken(winlogon) failed", GetLastError());
    }
    CloseHandle(hProc);
    return ok;
}

// Start the TrustedInstaller service; return its process PID (0 on failure).
inline DWORD StartTrustedInstaller(std::wstring& err) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { err = FormatErr(L"OpenSCManager failed", GetLastError()); return 0; }
    SC_HANDLE svc = OpenServiceW(scm, L"TrustedInstaller",
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    DWORD pid = 0;
    if (svc) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (!StartServiceW(svc, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING) { /* keep polling anyway */ }
        }
        for (int i = 0; i < 50; ++i) {
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<LPBYTE>(&ssp),
                                     sizeof(ssp), &needed)) {
                if (ssp.dwCurrentState == SERVICE_RUNNING && ssp.dwProcessId) {
                    pid = ssp.dwProcessId;
                    break;
                }
            }
            Sleep(100);
        }
        CloseServiceHandle(svc);
    } else {
        err = FormatErr(L"OpenService(TrustedInstaller) failed", GetLastError());
    }
    CloseServiceHandle(scm);
    if (!pid) pid = FindProcessId(L"TrustedInstaller.exe");
    if (!pid && err.empty()) err = L"Could not obtain TrustedInstaller PID.";
    return pid;
}

// Launch cmdline (optionally in workDir) under the TrustedInstaller token.
inline bool LaunchWithToken(DWORD tiPid, const std::wstring& cmdline,
                            const std::wstring& workDir, DWORD& outPid,
                            std::wstring& err) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, tiPid);
    if (!hProc) {
        err = FormatErr(L"OpenProcess(TrustedInstaller) failed", GetLastError());
        return false;
    }
    HANDLE hTok = nullptr, hDup = nullptr;
    bool ok = false;
    if (OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
        if (DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                             SecurityImpersonation, TokenPrimary, &hDup)) {
            // Enhanced: run with the full privilege set enabled, not just the
            // default TrustedInstaller identity.
            EnableAllPrivileges(hDup);
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.lpDesktop = const_cast<wchar_t*>(L"Winsta0\\Default");
            PROCESS_INFORMATION pi{};
            std::wstring buf = cmdline;  // CreateProcess* may modify the buffer
            const wchar_t* dir = workDir.empty() ? nullptr : workDir.c_str();
            if (CreateProcessWithTokenW(hDup, LOGON_WITH_PROFILE, nullptr,
                                        &buf[0],
                                        CREATE_UNICODE_ENVIRONMENT |
                                            CREATE_NEW_CONSOLE,
                                        nullptr, dir, &si, &pi)) {
                outPid = pi.dwProcessId;
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ok = true;
            } else {
                err = FormatErr(L"CreateProcessWithTokenW failed", GetLastError());
            }
            CloseHandle(hDup);
        } else {
            err = FormatErr(L"DuplicateTokenEx(TrustedInstaller) failed",
                            GetLastError());
        }
        CloseHandle(hTok);
    } else {
        err = FormatErr(L"OpenProcessToken(TrustedInstaller) failed",
                        GetLastError());
    }
    CloseHandle(hProc);
    return ok;
}

// Whole flow: run cmdline as TrustedInstaller. Restores our thread token after.
inline bool RunAsTI(const std::wstring& cmdline, const std::wstring& workDir,
                    DWORD& outPid, std::wstring& err) {
    err.clear();
    if (!EnableDebugPrivilege(err)) return false;
    if (!ImpersonateSystem(err)) return false;
    DWORD tiPid = StartTrustedInstaller(err);
    if (!tiPid) { RevertToSelf(); return false; }
    bool ok = LaunchWithToken(tiPid, cmdline, workDir, outPid, err);
    RevertToSelf();
    return ok;
}

}  // namespace ti

#endif  // EXECTI_TRUSTEDINSTALLER_H
