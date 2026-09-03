// ============================================================================
//  trustedinstaller.h - privilege launch engine (header-only)
// ----------------------------------------------------------------------------
//  Runs a target program under a chosen high-privilege token, using only public
//  Win32 token APIs. Shared by the console (execti-cli) and GUI (execti-gui)
//  front ends. Full option set (token source, integrity level, privileges,
//  console, working directory, wait) - a superset of the classic ExecTI, in the
//  spirit of NSudo.
//
//  Nothing here touches the kernel or bypasses a security control: it is
//  ordinary, documented token duplication + CreateProcessWithTokenW, gated by
//  the requireAdministrator manifest (UAC).
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

// ---- options --------------------------------------------------------------
enum class Source { CurrentUser, System, TrustedInstaller };

struct Options {
    Source       source        = Source::TrustedInstaller;
    DWORD        integrityRid   = 0;     // 0 = leave as-is; else SECURITY_MANDATORY_*_RID
    bool         enableAllPriv  = true;  // enable every privilege the token holds
    bool         newConsole     = true;  // give the child its own console
    bool         wait           = false; // block until the child exits
    std::wstring cmdline;
    std::wstring workDir;                // empty = default
};

// ---- small helpers --------------------------------------------------------
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

// Enable every privilege the token holds (SeDebug, SeTcb, SeLoadDriver, ...).
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

// Set a token's mandatory integrity level (best-effort; lowering only).
inline bool SetTokenIntegrity(HANDLE hToken, DWORD rid) {
    SID_IDENTIFIER_AUTHORITY mla = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID sid = nullptr;
    if (!AllocateAndInitializeSid(&mla, 1, rid, 0, 0, 0, 0, 0, 0, 0, &sid))
        return false;
    TOKEN_MANDATORY_LABEL tml{};
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    tml.Label.Sid = sid;
    BOOL ok = SetTokenInformation(hToken, TokenIntegrityLevel, &tml,
                                  sizeof(TOKEN_MANDATORY_LABEL) + GetLengthSid(sid));
    FreeSid(sid);
    return ok != FALSE;
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
            if (_wcsicmp(pe.szExeFile, imageName) == 0) { pid = pe.th32ProcessID; break; }
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
    if (!hProc) { err = FormatErr(L"OpenProcess(winlogon) failed", GetLastError()); return false; }
    HANDLE hTok = nullptr, hDup = nullptr;
    bool ok = false;
    if (OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
        if (DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                             SecurityImpersonation, TokenImpersonation, &hDup)) {
            if (SetThreadToken(nullptr, hDup)) ok = true;
            else err = FormatErr(L"SetThreadToken failed", GetLastError());
            CloseHandle(hDup);
        } else err = FormatErr(L"DuplicateTokenEx(winlogon) failed", GetLastError());
        CloseHandle(hTok);
    } else err = FormatErr(L"OpenProcessToken(winlogon) failed", GetLastError());
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
        StartServiceW(svc, 0, nullptr);  // ERROR_SERVICE_ALREADY_RUNNING is fine
        for (int i = 0; i < 50; ++i) {
            if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<LPBYTE>(&ssp),
                                     sizeof(ssp), &needed) &&
                ssp.dwCurrentState == SERVICE_RUNNING && ssp.dwProcessId) {
                pid = ssp.dwProcessId; break;
            }
            Sleep(100);
        }
        CloseServiceHandle(svc);
    } else err = FormatErr(L"OpenService(TrustedInstaller) failed", GetLastError());
    CloseServiceHandle(scm);
    if (!pid) pid = FindProcessId(L"TrustedInstaller.exe");
    if (!pid && err.empty()) err = L"Could not obtain TrustedInstaller PID.";
    return pid;
}

// Duplicate a process's token as a primary token.
inline HANDLE DupPrimaryTokenOf(DWORD pid, std::wstring& err) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) { err = FormatErr(L"OpenProcess failed", GetLastError()); return nullptr; }
    HANDLE hTok = nullptr, hDup = nullptr;
    if (OpenProcessToken(hProc, TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
        if (!DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                              SecurityImpersonation, TokenPrimary, &hDup))
            err = FormatErr(L"DuplicateTokenEx failed", GetLastError());
        CloseHandle(hTok);
    } else err = FormatErr(L"OpenProcessToken failed", GetLastError());
    CloseHandle(hProc);
    return hDup;
}

// Acquire the primary token for the requested source.
inline HANDLE AcquireToken(Source src, std::wstring& err) {
    if (src == Source::CurrentUser) {
        HANDLE hTok = nullptr, hDup = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hTok)) {
            if (!DuplicateTokenEx(hTok, TOKEN_ALL_ACCESS, nullptr,
                                  SecurityImpersonation, TokenPrimary, &hDup))
                err = FormatErr(L"DuplicateTokenEx(self) failed", GetLastError());
            CloseHandle(hTok);
        } else err = FormatErr(L"OpenProcessToken(self) failed", GetLastError());
        return hDup;
    }
    if (src == Source::System) {
        DWORD pid = FindProcessId(L"winlogon.exe");
        if (!pid) { err = L"winlogon.exe not found."; return nullptr; }
        return DupPrimaryTokenOf(pid, err);
    }
    // TrustedInstaller
    DWORD pid = StartTrustedInstaller(err);
    if (!pid) return nullptr;
    return DupPrimaryTokenOf(pid, err);
}

// ---- the launch --------------------------------------------------------
inline bool Run(const Options& opt, DWORD& outPid, std::wstring& err) {
    err.clear();
    outPid = 0;
    if (!EnableDebugPrivilege(err)) return false;
    // Impersonating SYSTEM grants SeImpersonate/SeAssignPrimaryToken/SeTcb, which
    // CreateProcessWithTokenW and cross-process token access need.
    if (!ImpersonateSystem(err)) return false;

    HANDLE hTok = AcquireToken(opt.source, err);
    if (!hTok) { RevertToSelf(); return false; }

    if (opt.enableAllPriv) EnableAllPrivileges(hTok);
    if (opt.integrityRid)  SetTokenIntegrity(hTok, opt.integrityRid);  // best-effort

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<wchar_t*>(L"Winsta0\\Default");
    PROCESS_INFORMATION pi{};
    std::wstring buf = opt.cmdline;  // CreateProcess* may modify the buffer
    const wchar_t* dir = opt.workDir.empty() ? nullptr : opt.workDir.c_str();
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (opt.newConsole ? CREATE_NEW_CONSOLE : 0);

    bool ok = false;
    if (CreateProcessWithTokenW(hTok, LOGON_WITH_PROFILE, nullptr, &buf[0],
                                flags, nullptr, dir, &si, &pi)) {
        outPid = pi.dwProcessId;
        if (opt.wait) WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ok = true;
    } else {
        err = FormatErr(L"CreateProcessWithTokenW failed", GetLastError());
    }
    CloseHandle(hTok);
    RevertToSelf();
    return ok;
}

// Back-compat convenience: run cmdline as TrustedInstaller with all privileges.
inline bool RunAsTI(const std::wstring& cmdline, const std::wstring& workDir,
                    DWORD& outPid, std::wstring& err) {
    Options o;
    o.source = Source::TrustedInstaller;
    o.cmdline = cmdline;
    o.workDir = workDir;
    return Run(o, outPid, err);
}

}  // namespace ti

#endif  // EXECTI_TRUSTEDINSTALLER_H
