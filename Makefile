# GooberBASIC build
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf SDL2_mixer)
SDL_LIBS   := $(shell pkg-config --libs sdl2 SDL2_image SDL2_ttf SDL2_mixer)
LDLIBS  += $(SDL_LIBS) -lm

SRC := src/main.c src/lexer.c src/compiler.c src/vm.c src/objects.c src/builtins.c src/net.c src/gfx.c
OBJ := $(SRC:src/%.c=build/%.o)

all: goober

goober: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

build:
	mkdir -p build

test: goober
	./tests/run_tests.sh

# regenerate the VS Code grammar from the compiler's keyword and command tables
vscode:
	python3 editors/vscode/generate.py

clean:
	rm -rf build goober

.PHONY: all test clean vscode
