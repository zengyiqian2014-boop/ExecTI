# Prebuilt binaries

Ready-to-run Windows executables, cross-compiled with MinGW. Run **as
Administrator** (they request UAC elevation on launch).

| File | Front end | Architecture |
| ---- | --------- | ------------ |
| `execti-gui-x86_64.exe` | GUI (Run-style dialog) | Intel / AMD (x64) |
| `execti-gui-arm64.exe`  | GUI (Run-style dialog) | Windows on ARM64 |
| `execti-x86_64.exe`     | Console | Intel / AMD (x64) |
| `execti-arm64.exe`      | Console | Windows on ARM64 |

`SHA256SUMS.txt` holds the checksums — verify with `sha256sum -c SHA256SUMS.txt`
(or `CertUtil -hashfile <file> SHA256` on Windows).

> Not sure which one? Most PCs are **x86_64** → use `execti-gui-x86_64.exe`.
> ARM devices (e.g. Snapdragon laptops, Windows on ARM VMs) → the `arm64` builds.

These are built from the sources in [`../src`](../src). To rebuild them yourself
see the top-level [README](../README.md) (`make` / `build.sh`), or download them
from the repository's GitHub **Releases** page.
