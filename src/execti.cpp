// ============================================================================
//  execti-cli - console front end for the privilege launch engine
// ----------------------------------------------------------------------------
//  Usage:  execti-cli [options] <program> [args...]
//
//  Options:
//    -u, --user <ti|system|admin>     token source (default: ti)
//    -i, --integrity <system|high|medium|low>   integrity level (default: system)
//    -p, --no-priv                    do NOT enable all privileges
//    -d, --cwd <dir>                  working directory
//    -w, --wait                       wait for the launched process to exit
//        --no-console                 do not give the child a new console
//    -h, --help                       show help
//
//  With no <program>, opens cmd.exe. Must be run elevated (Administrator).
// ============================================================================

#include "trustedinstaller.h"

#include <cstdio>
#include <string>

static void usage() {
    fwprintf(stderr,
        L"ExecTI CLI - run a program with high privileges\n\n"
        L"Usage: execti-cli [options] <program> [args...]\n\n"
        L"  -u, --user <ti|system|admin>   token source (default: ti)\n"
        L"  -i, --integrity <system|high|medium|low>  (default: system)\n"
        L"  -p, --no-priv                  do not enable all privileges\n"
        L"  -d, --cwd <dir>                working directory\n"
        L"  -w, --wait                     wait for the process to exit\n"
        L"      --no-console               reuse this console (no new window)\n"
        L"  -h, --help                     show this help\n\n"
        L"With no program, opens cmd.exe as TrustedInstaller.\n");
}

static void quoteInto(std::wstring& out, const std::wstring& a) {
    if (!out.empty()) out += L' ';
    if (a.find(L' ') != std::wstring::npos && (a.empty() || a.front() != L'"'))
        out += L'"' + a + L'"';
    else
        out += a;
}

int wmain(int argc, wchar_t** argv) {
    ti::Options o;
    std::wstring cmd;

    int i = 1;
    for (; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 >= argc) { fwprintf(stderr, L"[!] %s needs a value\n", name); exit(2); }
            return argv[++i];
        };
        if (a == L"-h" || a == L"--help") { usage(); return 0; }
        else if (a == L"-u" || a == L"--user") {
            std::wstring v = next(L"--user");
            if (v == L"ti" || v == L"trustedinstaller") o.source = ti::Source::TrustedInstaller;
            else if (v == L"system" || v == L"sys")     o.source = ti::Source::System;
            else if (v == L"admin" || v == L"user")     o.source = ti::Source::CurrentUser;
            else { fwprintf(stderr, L"[!] unknown user: %s\n", v.c_str()); return 2; }
        }
        else if (a == L"-i" || a == L"--integrity") {
            std::wstring v = next(L"--integrity");
            if (v == L"system") o.integrityRid = SECURITY_MANDATORY_SYSTEM_RID;
            else if (v == L"high")   o.integrityRid = SECURITY_MANDATORY_HIGH_RID;
            else if (v == L"medium") o.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
            else if (v == L"low")    o.integrityRid = SECURITY_MANDATORY_LOW_RID;
            else { fwprintf(stderr, L"[!] unknown integrity: %s\n", v.c_str()); return 2; }
        }
        else if (a == L"-p" || a == L"--no-priv") o.enableAllPriv = false;
        else if (a == L"-d" || a == L"--cwd")     o.workDir = next(L"--cwd");
        else if (a == L"-w" || a == L"--wait")    o.wait = true;
        else if (a == L"--no-console")            o.newConsole = false;
        else if (a == L"--") { ++i; break; }
        else if (!a.empty() && a[0] == L'-') {
            fwprintf(stderr, L"[!] unknown option: %s\n", a.c_str()); return 2;
        }
        else break;  // first non-option = start of the command
    }
    for (; i < argc; ++i) quoteInto(cmd, argv[i]);
    if (cmd.empty()) cmd = L"cmd.exe";
    o.cmdline = cmd;

    const wchar_t* srcName = o.source == ti::Source::TrustedInstaller ? L"TrustedInstaller"
                           : o.source == ti::Source::System           ? L"SYSTEM"
                                                                       : L"current user";
    fwprintf(stderr, L"[*] ExecTI - launching as %s: %s\n", srcName, cmd.c_str());

    DWORD pid = 0;
    std::wstring err;
    if (ti::Run(o, pid, err)) {
        fwprintf(stderr, L"[+] Started PID %lu\n", pid);
        return 0;
    }
    fwprintf(stderr, L"[!] %s\n", err.c_str());
    return 1;
}
