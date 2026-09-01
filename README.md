# ExecTI (C++)

A small, dependency-free reimplementation of the well-known **ExecTI** / **RunAsTI**
utility. It launches any program with **TrustedInstaller** privileges — the security
context that owns Windows' OS-protected files and registry keys (TrustedInstaller
outranks *Administrator* and even *SYSTEM* on those objects).

Two front ends, cross-compiled with **MinGW** for both **x86_64** and **ARM64** Windows.

### GUI vs CLI — which one?

Both do the exact same thing (run a program as TrustedInstaller); they differ only
in how you drive them:

| | **GUI** — `execti-gui-*.exe` | **CLI** — `execti-cli-*.exe` |
| --- | --- | --- |
| Interface | A graphical **"Run" dialog** (like Win+R) | A **console** command |
| How you use it | **Double-click**, type or browse, click Run | Type `execti-cli <program>` in a terminal / script |
| Picking the target | Editable box + **Browse** (file) + **Folder** buttons | Pass it as an argument |
| History | **Dropdown ▼** remembers recent entries (saved in registry) | — (use your shell history) |
| Best for | Everyday interactive use | Automation, scripts, `.bat` files, remote shells |
| Window | Yes | No (runs and exits) |

If you're not sure, use the **GUI**.

**⬇ Just want the program?** Prebuilt `.exe` files are in [`bin/`](bin/) (and on the
GitHub **Releases** page). Most PCs → `bin/execti-gui-x86_64.exe`.

> ⚠️ **Use responsibly.** This is a system-administration / security-research tool.
> Running as TrustedInstaller lets you overwrite protected OS files and registry
> keys — a mistake can render Windows unbootable. Only run it on systems you own
> or are authorized to administer.

---

## What it does

### GUI — `execti-gui.exe`

Double-click it (accept the UAC prompt) and you get a small Run-style window:

```
 🛡  Type a program, folder, or file name, and ExecTI will
     run it with TrustedInstaller privileges.

 Open: [ regedit                              ▼ ]   <- dropdown = history
        [ Run ]  [ Cancel ]  [ Browse... ]  [ Folder... ]
```

- Type a command (`regedit`, `cmd`, `notepad C:\Windows\System32\drivers\etc\hosts`),
  or a full path, or `%windir%`-style variables — then press **Run** / Enter.
- Click the **▼ arrow** on the right of the box to drop down the list of things you
  ran before (kept in `HKCU\Software\ExecTI`, most-recent first, up to 20 entries).
- **Browse** opens a file picker; **Folder** opens a folder picker (a folder is
  opened in Explorer as TrustedInstaller).

### CLI — `execti-cli.exe`

`execti-cli.exe [program] [args...]` — with no arguments it opens an elevated
`cmd.exe` running as `NT SERVICE\TrustedInstaller`. Verify inside that shell with
`whoami /groups` — you'll see the `S-1-5-80-956008885-...` TrustedInstaller SID with
*Owner* / *Enabled* flags.

```
execti-cli.exe                          # TrustedInstaller command prompt
execti-cli.exe regedit.exe              # registry editor as TrustedInstaller
execti-cli.exe cmd.exe /c "del C:\Windows\System32\some_protected_file"
```

## How it works

The classic, fully documented token-manipulation technique — no exploits, only
public Win32 APIs, and it requires you to already be an Administrator:

1. **Enable `SeDebugPrivilege`** on our own token (`AdjustTokenPrivileges`). This is
   why the app ships with a `requireAdministrator` manifest — Windows shows a UAC
   prompt on launch.
2. **Become SYSTEM.** Find `winlogon.exe`, open its token, `DuplicateTokenEx` it as an
   impersonation token, and `SetThreadToken` — our thread now runs as SYSTEM, which
   carries `SeImpersonatePrivilege`.
3. **Start the TrustedInstaller service** through the Service Control Manager
   (`OpenSCManager` → `OpenService("TrustedInstaller")` → `StartService`), then read the
   spawned `TrustedInstaller.exe` PID from `QueryServiceStatusEx`.
4. **Duplicate that process's token** as a primary token (`OpenProcessToken` +
   `DuplicateTokenEx(TokenPrimary)`).
5. **Spawn the target** with `CreateProcessWithTokenW` using the duplicated token, so
   the child process runs as TrustedInstaller.

See [`src/execti.cpp`](src/execti.cpp) — every step is commented.

## Building

### Toolchains

| Target | Toolchain | Provides |
| ------ | --------- | -------- |
| x86_64 | [mingw-w64](https://www.mingw-w64.org/) GCC | `x86_64-w64-mingw32-g++` |
| ARM64  | [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang) | `aarch64-w64-mingw32-clang++` |

> mingw-w64's GCC does **not** target Windows-on-ARM64. LLVM's `llvm-mingw`
> distribution is the practical way to get an `aarch64-w64-mingw32` cross-compiler.

**Debian / Ubuntu:**

```bash
# x86_64 toolchain
sudo apt-get install mingw-w64

# ARM64 toolchain — grab a prebuilt llvm-mingw release and put it on PATH
curl -fsSL -o llvm-mingw.tar.xz \
  https://github.com/mstorsjo/llvm-mingw/releases/download/20250528/llvm-mingw-20250528-ucrt-ubuntu-22.04-x86_64.tar.xz
sudo tar -C /opt -xf llvm-mingw.tar.xz && sudo mv /opt/llvm-mingw-* /opt/llvm-mingw
export PATH="/opt/llvm-mingw/bin:$PATH"
```

### Compile

```bash
make            # both architectures
make x64        # -> build/x86_64/execti-cli.exe + execti-gui.exe
make arm64      # -> build/arm64/execti-cli.exe + execti-gui.exe

# or the convenience script (auto-detects /opt/llvm-mingw):
./build.sh
```

Output:

```
build/x86_64/execti-cli.exe   CLI (console), x86-64
build/x86_64/execti-gui.exe   GUI,           x86-64
build/arm64/execti-cli.exe    CLI (console), Aarch64
build/arm64/execti-gui.exe    GUI,           Aarch64
```

Both are statically linked (`-static-libgcc -static-libstdc++`) so they depend only
on system DLLs (`kernel32`, `advapi32`) plus the Universal CRT that ships with
Windows 10/11. The embedded manifest triggers the UAC elevation prompt.

## Releases

Prebuilt binaries are published automatically. Push a version tag and CI builds
both architectures and attaches them (plus a combined zip and `SHA256SUMS.txt`) to
a GitHub Release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

You can also trigger it manually from the **Actions → release** tab
(`workflow_dispatch`) with a tag name. See
[`.github/workflows/release.yml`](.github/workflows/release.yml).

## Project layout

```
src/trustedinstaller.h  shared TrustedInstaller launch logic (header-only)
src/execti.cpp          CLI (console) front end
src/execti_gui.cpp      GUI front end (Run dialog + history + browse)
src/execti.rc           CLI version info + manifest resource
src/execti_gui.rc       GUI version info + manifest resource
src/execti.manifest     requireAdministrator (UAC) + themed controls + dpiAware
src/execti.ico          application icon (embedded in both .exe files)
Makefile                cross-compile rules: 2 front ends x (x64 + arm64)
build.sh                convenience wrapper
bin/                    prebuilt binaries (execti-cli-* / execti-gui-*)
.github/workflows/      CI + release automation
```

## References / further reading

- **Sordum ExecTI** — the original utility this reimplements:
  https://www.sordum.org/9416/powerrun-v1-8-run-with-highest-privileges/ (PowerRun /
  ExecTI family)
- **RunAsTI** by Vadim Sterkin — the widely-cited reference implementation of the
  winlogon→SYSTEM→TrustedInstaller token flow:
  https://github.com/jschicht/RunAsTI (and the writeups it links)
- **Microsoft Learn — token & privilege APIs:**
  - [`DuplicateTokenEx`](https://learn.microsoft.com/windows/win32/api/securitybaseapi/nf-securitybaseapi-duplicatetokenex)
  - [`CreateProcessWithTokenW`](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-createprocesswithtokenw)
  - [`AdjustTokenPrivileges`](https://learn.microsoft.com/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges)
  - [`QueryServiceStatusEx`](https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-queryservicestatusex)
- **NSudo / NSudoLM** — a larger open-source tool in the same space, useful for
  comparing approaches: https://github.com/M2Team/NSudo
- **llvm-mingw** — the ARM64 Windows cross-toolchain:
  https://github.com/mstorsjo/llvm-mingw

## License

Provided as-is for educational and administrative use. No warranty.
