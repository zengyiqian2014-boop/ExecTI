// ============================================================================
//  execti - console front end: run a program with TrustedInstaller privileges
// ----------------------------------------------------------------------------
//  Usage:  execti.exe [program] [args...]
//  With no arguments it opens an elevated cmd.exe running as TrustedInstaller.
//
//  The token-manipulation logic lives in trustedinstaller.h and is shared with
//  the GUI front end (execti-gui). Must be run elevated (Administrator).
// ============================================================================

#include "trustedinstaller.h"

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    // Default target: an elevated interactive command prompt.
    std::wstring cmdline = L"cmd.exe";
    if (argc > 1) {
        cmdline.clear();
        for (int i = 1; i < argc; ++i) {
            if (i > 1) cmdline += L' ';
            std::wstring a = argv[i];
            if (a.find(L' ') != std::wstring::npos && a.front() != L'"')
                cmdline += L'"' + a + L'"';
            else
                cmdline += a;
        }
    }

    fwprintf(stderr, L"[*] ExecTI - launching: %s\n", cmdline.c_str());

    DWORD pid = 0;
    std::wstring err;
    if (ti::RunAsTI(cmdline, L"", pid, err)) {
        fwprintf(stderr, L"[+] Started PID %lu as TrustedInstaller\n", pid);
        return 0;
    }
    fwprintf(stderr, L"[!] %s\n", err.c_str());
    return 1;
}
