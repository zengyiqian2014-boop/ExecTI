# Prebuilt binaries

Ready-to-run Windows executables, cross-compiled with MinGW. Run **as
Administrator** (they request UAC elevation on launch).

| File | Front end | Architecture |
| ---- | --------- | ------------ |
| `execti-gui-x86_64.exe` | **GUI** (Run-style dialog) | Intel / AMD (x64) |
| `execti-gui-arm64.exe`  | **GUI** (Run-style dialog) | Windows on ARM64 |
| `execti-cli-x86_64.exe` | **CLI** (console) | Intel / AMD (x64) |
| `execti-cli-arm64.exe`  | **CLI** (console) | Windows on ARM64 |

**GUI vs CLI** — same power, different interface:

- **GUI** (`execti-gui-*`) — a graphical "Run" box: type or browse for a program,
  pick a folder, and a dropdown remembers what you ran before. Just double-click it.
- **CLI** (`execti-cli-*`) — a command-line tool for scripts / terminals:
  `execti-cli regedit`. No window; runs the target and exits.

`SHA256SUMS.txt` holds the checksums — verify with `sha256sum -c SHA256SUMS.txt`
(or `CertUtil -hashfile <file> SHA256` on Windows).

> Not sure which one? Most people want **`execti-gui-x86_64.exe`**.
> ARM devices (Snapdragon laptops, Windows-on-ARM VMs) → the `arm64` builds.

Built from the sources in [`../src`](../src). To rebuild them yourself see the
top-level [README](../README.md) (`make` / `build.sh`), or download them from the
repository's GitHub **Releases** page.
