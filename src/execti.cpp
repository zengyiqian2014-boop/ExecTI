// ============================================================================
//  ExecTI - run a program with TrustedInstaller privileges
// ----------------------------------------------------------------------------
//  A small, dependency-free reimplementation of the well-known ExecTI /
//  RunAsTI utility. It launches a target program under the security context of
//  the Windows "TrustedInstaller" service account, which owns OS-protected
//  files and registry keys (TrustedInstaller sits above Administrator and even
//  SYSTEM for those objects).
//
//  Technique (the classic, fully documented approach):
//    1. Enable SeDebugPrivilege on our own token (requires Administrator).
//    2. Steal a SYSTEM token by impersonating winlogon.exe, so our thread
//       runs as SYSTEM (this grants SeImpersonatePrivilege etc.).
//    3. Start the "TrustedInstaller" service via the SCM and read the PID of
//       the resulting TrustedInstaller.exe from the service status.
//    4. Open that process, duplicate its primary token.
//    5. CreateProcessWithTokenW() with the duplicated token to spawn the
//       target program as TrustedInstaller.
//
//  Must be run elevated (as Administrator). Windows only.
//
//  This is a system-administration / research tool. Modifying OS-protected
//  resources can break your Windows install - use with care.
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <string>

// llvm-mingw / mingw-w64 both ship these, but define fallbacks just in case.
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

static void logf(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    va_end(ap);
    fputws(buf, stderr);
    fputws(L"\n", stderr);
}

// Enable a named privilege on a token (e.g. SeDebugPrivilege).
static bool EnablePrivilege(HANDLE hToken, const wchar_t* name) {
    TOKEN_PRIVILEGES tp{};
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        logf(L"[!] LookupPrivilegeValue(%s) failed: %lu", name, GetLastError());
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        logf(L"[!] AdjustTokenPrivileges(%s) failed: %lu", name, GetLastError());
        return false;
    }
    return GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

// Enable SeDebugPrivilege on our own process token.
static bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        logf(L"[!] OpenProcessToken(self) failed: %lu", GetLastError());
        return false;
    }
    bool ok = EnablePrivilege(hToken, SE_DEBUG_NAME);
    CloseHandle(hToken);
    return ok;
}

// Find the PID of the first process whose image name matches (case-insensitive).
static DWORD FindProcessId(const wchar_t* imageName) {
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
static bool ImpersonateSystem() {
    DWORD pid = FindProcessId(L"winlogon.exe");
    if (!pid) {
        logf(L"[!] winlogon.exe not found");
        return false;
    }
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        logf(L"[!] OpenProcess(winlogon %lu) failed: %lu", pid, GetLastError());
        return false;
    }
    HANDLE hTok = nullptr, hDup = nullptr;
    bool ok = false;
    if (OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
        if (DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                             SecurityImpersonation, TokenImpersonation, &hDup)) {
            if (SetThreadToken(nullptr, hDup)) {
                ok = true;
            } else {
                logf(L"[!] SetThreadToken failed: %lu", GetLastError());
            }
            CloseHandle(hDup);
        } else {
            logf(L"[!] DuplicateTokenEx(winlogon) failed: %lu", GetLastError());
        }
        CloseHandle(hTok);
    } else {
        logf(L"[!] OpenProcessToken(winlogon) failed: %lu", GetLastError());
    }
    CloseHandle(hProc);
    return ok;
}

// Start the TrustedInstaller service and return the PID of TrustedInstaller.exe.
static DWORD StartTrustedInstaller() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        logf(L"[!] OpenSCManager failed: %lu", GetLastError());
        return 0;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"TrustedInstaller",
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    DWORD pid = 0;
    if (svc) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        // Kick the service; ERROR_SERVICE_ALREADY_RUNNING is fine.
        if (!StartServiceW(svc, 0, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_SERVICE_ALREADY_RUNNING)
                logf(L"[i] StartService returned %lu (continuing)", e);
        }
        // Poll until the service reports RUNNING with a valid PID.
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
        logf(L"[!] OpenService(TrustedInstaller) failed: %lu", GetLastError());
    }
    CloseServiceHandle(scm);
    if (!pid) pid = FindProcessId(L"TrustedInstaller.exe");
    return pid;
}

// Launch the target command line under the TrustedInstaller process token.
static bool LaunchAsTrustedInstaller(DWORD tiPid, std::wstring cmdline) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, tiPid);
    if (!hProc) {
        logf(L"[!] OpenProcess(TrustedInstaller %lu) failed: %lu", tiPid,
             GetLastError());
        return false;
    }
    HANDLE hTok = nullptr, hDup = nullptr;
    bool ok = false;
    if (OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
        if (DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                             SecurityImpersonation, TokenPrimary, &hDup)) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.lpDesktop = const_cast<wchar_t*>(L"Winsta0\\Default");
            PROCESS_INFORMATION pi{};
            // Writable buffer for the command line (CreateProcess* may modify it).
            std::wstring buf = cmdline;
            if (CreateProcessWithTokenW(hDup, LOGON_WITH_PROFILE, nullptr,
                                        &buf[0],
                                        CREATE_UNICODE_ENVIRONMENT |
                                            CREATE_NEW_CONSOLE,
                                        nullptr, nullptr, &si, &pi)) {
                logf(L"[+] Started PID %lu as TrustedInstaller", pi.dwProcessId);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ok = true;
            } else {
                logf(L"[!] CreateProcessWithTokenW failed: %lu", GetLastError());
            }
            CloseHandle(hDup);
        } else {
            logf(L"[!] DuplicateTokenEx(TrustedInstaller) failed: %lu",
                 GetLastError());
        }
        CloseHandle(hTok);
    } else {
        logf(L"[!] OpenProcessToken(TrustedInstaller) failed: %lu",
             GetLastError());
    }
    CloseHandle(hProc);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    // Default target: an elevated interactive command prompt.
    std::wstring cmdline = L"cmd.exe";
    if (argc > 1) {
        cmdline.clear();
        for (int i = 1; i < argc; ++i) {
            if (i > 1) cmdline += L' ';
            // Quote arguments that contain spaces and aren't already quoted.
            std::wstring a = argv[i];
            if (a.find(L' ') != std::wstring::npos && a.front() != L'"')
                cmdline += L'"' + a + L'"';
            else
                cmdline += a;
        }
    }

    logf(L"[*] ExecTI - launching: %s", cmdline.c_str());

    if (!EnableDebugPrivilege()) {
        logf(L"[!] Could not enable SeDebugPrivilege. Run as Administrator.");
        return 1;
    }
    if (!ImpersonateSystem()) {
        logf(L"[!] Failed to impersonate SYSTEM.");
        return 2;
    }
    DWORD tiPid = StartTrustedInstaller();
    if (!tiPid) {
        logf(L"[!] Could not obtain TrustedInstaller PID.");
        RevertToSelf();
        return 3;
    }
    logf(L"[*] TrustedInstaller PID: %lu", tiPid);

    bool ok = LaunchAsTrustedInstaller(tiPid, cmdline);
    RevertToSelf();  // drop the SYSTEM impersonation on our thread
    return ok ? 0 : 4;
}
