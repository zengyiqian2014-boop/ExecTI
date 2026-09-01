# ============================================================================
#  ExecTI - cross-compile with MinGW for Windows x86_64 and ARM64
# ============================================================================
#
#  Builds two front ends, for two architectures (4 binaries total):
#    execti-cli.exe  - console tool:  execti-cli.exe [program] [args...]
#    execti-gui.exe  - Run-style GUI with history dropdown + Browse / Folder
#
#  Toolchains
#  ----------
#    x86_64 : mingw-w64 GCC     (x86_64-w64-mingw32-g++ / -windres)
#    arm64  : llvm-mingw Clang  (aarch64-w64-mingw32-clang++ / -windres)
#
#  mingw-w64 GCC does not target Windows-on-ARM64; use LLVM's llvm-mingw
#  (https://github.com/mstorsjo/llvm-mingw) for the aarch64 target.
#
#  Usage
#  -----
#    make            # build everything (both front ends, both arches)
#    make x64        # both front ends for x86_64  -> build/x86_64/
#    make arm64      # both front ends for ARM64   -> build/arm64/
#    make clean
# ============================================================================

# --- x86_64 (mingw-w64 GCC) -------------------------------------------------
CXX_X64      ?= x86_64-w64-mingw32-g++
WINDRES_X64  ?= x86_64-w64-mingw32-windres

# --- ARM64 (llvm-mingw Clang) -----------------------------------------------
CXX_ARM64      ?= aarch64-w64-mingw32-clang++
WINDRES_ARM64  ?= aarch64-w64-mingw32-windres

# --- flags ------------------------------------------------------------------
CXXFLAGS   := -std=c++17 -O2 -municode -Wall -Wextra -Isrc
# Static-link the runtimes so the .exe has no external runtime DLL dependencies.
LDBASE     := -static -static-libgcc -static-libstdc++ -municode
LIBS_CON   := -ladvapi32 -lkernel32
# GUI also needs the common/comdlg/shell libraries.
LIBS_GUI   := -ladvapi32 -lkernel32 -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid

ICON       := src/execti.ico
BUILD      := build
X64_DIR    := $(BUILD)/x86_64
ARM64_DIR  := $(BUILD)/arm64

.PHONY: all x64 arm64 clean
all: x64 arm64

x64:   $(X64_DIR)/execti-cli.exe   $(X64_DIR)/execti-gui.exe
arm64: $(ARM64_DIR)/execti-cli.exe $(ARM64_DIR)/execti-gui.exe

# ------------------------------- x86_64 -------------------------------------
$(X64_DIR)/cli-res.o: src/execti.rc src/execti.manifest $(ICON) | $(X64_DIR)
	$(WINDRES_X64) -I src -O coff src/execti.rc $@
$(X64_DIR)/gui-res.o: src/execti_gui.rc src/execti.manifest $(ICON) | $(X64_DIR)
	$(WINDRES_X64) -I src -O coff src/execti_gui.rc $@

$(X64_DIR)/execti-cli.exe: src/execti.cpp src/trustedinstaller.h $(X64_DIR)/cli-res.o | $(X64_DIR)
	$(CXX_X64) $(CXXFLAGS) src/execti.cpp $(X64_DIR)/cli-res.o \
		$(LDBASE) -Wl,--subsystem,console $(LIBS_CON) -o $@
	@echo "[x86_64] -> $@"

$(X64_DIR)/execti-gui.exe: src/execti_gui.cpp src/trustedinstaller.h $(X64_DIR)/gui-res.o | $(X64_DIR)
	$(CXX_X64) $(CXXFLAGS) src/execti_gui.cpp $(X64_DIR)/gui-res.o \
		$(LDBASE) -mwindows $(LIBS_GUI) -o $@
	@echo "[x86_64] -> $@"

# ------------------------------- ARM64 --------------------------------------
$(ARM64_DIR)/cli-res.o: src/execti.rc src/execti.manifest $(ICON) | $(ARM64_DIR)
	$(WINDRES_ARM64) -I src -O coff src/execti.rc $@
$(ARM64_DIR)/gui-res.o: src/execti_gui.rc src/execti.manifest $(ICON) | $(ARM64_DIR)
	$(WINDRES_ARM64) -I src -O coff src/execti_gui.rc $@

$(ARM64_DIR)/execti-cli.exe: src/execti.cpp src/trustedinstaller.h $(ARM64_DIR)/cli-res.o | $(ARM64_DIR)
	$(CXX_ARM64) $(CXXFLAGS) src/execti.cpp $(ARM64_DIR)/cli-res.o \
		$(LDBASE) -Wl,--subsystem,console $(LIBS_CON) -o $@
	@echo "[arm64]  -> $@"

$(ARM64_DIR)/execti-gui.exe: src/execti_gui.cpp src/trustedinstaller.h $(ARM64_DIR)/gui-res.o | $(ARM64_DIR)
	$(CXX_ARM64) $(CXXFLAGS) src/execti_gui.cpp $(ARM64_DIR)/gui-res.o \
		$(LDBASE) -mwindows $(LIBS_GUI) -o $@
	@echo "[arm64]  -> $@"

$(X64_DIR) $(ARM64_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
