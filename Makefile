# ============================================================================
#  ExecTI - cross-compile with MinGW for Windows x86_64 and ARM64
# ============================================================================
#
#  Toolchains
#  ----------
#    x86_64 : mingw-w64 GCC     (x86_64-w64-mingw32-g++ / -windres)
#    arm64  : llvm-mingw Clang  (aarch64-w64-mingw32-clang++ / -windres)
#
#  mingw-w64 GCC does not target Windows-on-ARM64; use LLVM's llvm-mingw
#  (https://github.com/mstorsjo/llvm-mingw) for the aarch64 target. Put its
#  bin/ on PATH, or override the *_ARM64 variables below.
#
#  Usage
#  -----
#    make            # build both architectures
#    make x64        # build only x86_64  -> build/x86_64/execti.exe
#    make arm64      # build only ARM64   -> build/arm64/execti.exe
#    make clean
#
#  Override a compiler if it isn't on PATH, e.g.:
#    make CXX_ARM64=/opt/llvm-mingw/bin/aarch64-w64-mingw32-clang++ \
#         WINDRES_ARM64=/opt/llvm-mingw/bin/aarch64-w64-mingw32-windres
# ============================================================================

SRC        := src/execti.cpp
RC         := src/execti.rc
OUT        := execti.exe

# --- x86_64 (mingw-w64 GCC) -------------------------------------------------
CXX_X64      ?= x86_64-w64-mingw32-g++
WINDRES_X64  ?= x86_64-w64-mingw32-windres

# --- ARM64 (llvm-mingw Clang) -----------------------------------------------
CXX_ARM64      ?= aarch64-w64-mingw32-clang++
WINDRES_ARM64  ?= aarch64-w64-mingw32-windres

# --- flags ------------------------------------------------------------------
CXXFLAGS   := -std=c++17 -O2 -municode -Wall -Wextra
# Static-link the runtimes so the .exe has no external DLL dependencies.
LDFLAGS    := -static -static-libgcc -static-libstdc++ \
              -municode -Wl,--subsystem,console
LIBS       := -ladvapi32 -lkernel32

BUILD      := build
X64_DIR    := $(BUILD)/x86_64
ARM64_DIR  := $(BUILD)/arm64

.PHONY: all x64 arm64 clean
all: x64 arm64

x64: $(X64_DIR)/$(OUT)
arm64: $(ARM64_DIR)/$(OUT)

# --- x86_64 -----------------------------------------------------------------
$(X64_DIR)/res.o: $(RC) src/execti.manifest | $(X64_DIR)
	$(WINDRES_X64) -I src -O coff $(RC) $@

$(X64_DIR)/$(OUT): $(SRC) $(X64_DIR)/res.o | $(X64_DIR)
	$(CXX_X64) $(CXXFLAGS) $(SRC) $(X64_DIR)/res.o $(LDFLAGS) $(LIBS) -o $@
	@echo "[x86_64] -> $@"

# --- ARM64 ------------------------------------------------------------------
$(ARM64_DIR)/res.o: $(RC) src/execti.manifest | $(ARM64_DIR)
	$(WINDRES_ARM64) -I src -O coff $(RC) $@

$(ARM64_DIR)/$(OUT): $(SRC) $(ARM64_DIR)/res.o | $(ARM64_DIR)
	$(CXX_ARM64) $(CXXFLAGS) $(SRC) $(ARM64_DIR)/res.o $(LDFLAGS) $(LIBS) -o $@
	@echo "[arm64]  -> $@"

$(X64_DIR) $(ARM64_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
