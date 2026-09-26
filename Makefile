# Super Mario Kart recompilation -- build rules (clang/g++ + make)
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
ROM      ?= Super Mario Kart (USA).sfc
BIN      := bin

TOOLS := $(BIN)/smk-disasm $(BIN)/smk-extract $(BIN)/smk-game $(BIN)/smk-lztrace

.PHONY: all tools clean info disasm assets game shot

all: tools

tools: $(TOOLS)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/smk-disasm: tools/disasm.cpp src/disasm.h src/rom.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ tools/disasm.cpp

$(BIN)/smk-extract: tools/extract.cpp src/rom.h src/png.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ tools/extract.cpp

$(BIN)/smk-map: tools/maptool.cpp src/rom.h src/png.h src/disasm.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ tools/maptool.cpp

# The recompiled runtime: Win32 + software mode-7 renderer, loads all of its
# assets from the user's ROM at startup (nothing embedded).
GAME_LIBS := -luser32 -lgdi32 -lcomdlg32
$(BIN)/smk-game: tools/game.cpp src/rom.h src/png.h src/smk/lz.h src/smk/course.h src/smk/kart.h src/smk/sprite.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ tools/game.cpp $(GAME_LIBS)

game: $(BIN)/smk-game

# Traces CODE_84E09E one command at a time: how each stream in the ROM decodes.
$(BIN)/smk-lztrace: tools/lztrace.cpp src/rom.h | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ tools/lztrace.cpp

# Render one frame headlessly so the output can be eyeballed as a PNG.
shot: $(BIN)/smk-game
	$(BIN)/smk-game "$(ROM)" --course 7 --frames 240 --shot build/shot.png

# Quick sanity pass: disassemble the reset path to a log file.
disasm: $(BIN)/smk-disasm
	$(BIN)/smk-disasm "$(ROM)" --reset -o build/reset.asm

assets: $(BIN)/smk-extract
	$(BIN)/smk-extract "$(ROM)" -o assets

info: $(BIN)/smk-disasm
	$(BIN)/smk-disasm --help

clean:
	rm -rf $(BIN) build
