
CC_ATARI  = m68k-atari-mint-gcc
CC_LINUX  = gcc
VASM      = vasm

LIBCMINI_DIR = /home/ben/src/atari/libcmini
LIBCMINI     = $(LIBCMINI_DIR)/build/mshort
CRT0         = $(LIBCMINI)/objs/crt0.o

VASMFLAGS    = -Faout -quiet -x -m68000 -spaces -showopt
CFLAGS       = -O2 -Wall -Wextra -Werror -g -std=gnu99
CFLAGS_ATARI = $(CFLAGS) -mshort -nostdlib -I$(LIBCMINI_DIR)/include
LDFLAGS_ATARI = -L$(LIBCMINI) -lcmini -lgcc -lcmini
LDFLAGS_LINUX = -lm

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

all: SV2025.tos SV2025-ascii.tos sv2025-sdl sv2025-ascii

clean:
	rm -f *.o *.tos *.sym sv2025-sdl sv2025-ascii

# ── Atari object files ─────────────────────────────────────────────────────────

SV2025_atari.o: SV2025.c backend.h SV2025.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

backend_gemtos.o: backend_gemtos.c backend.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

backend_ascii_atari.o: backend_ascii.c backend.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

segline.o: segmented-line.git/segline.s
	$(VASM) $(VASMFLAGS) $< -o $@

clipline.o: segmented-line.git/clipline.s
	$(VASM) $(VASMFLAGS) $< -o $@

# ── Linux object files ─────────────────────────────────────────────────────────

SV2025_linux.o: SV2025.c backend.h SV2025.h
	$(CC_LINUX) $(CFLAGS) -c $< -o $@

backend_sdl.o: backend_sdl.c backend.h
	$(CC_LINUX) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

backend_ascii_linux.o: backend_ascii.c backend.h
	$(CC_LINUX) $(CFLAGS) -c $< -o $@

# ── Link targets ───────────────────────────────────────────────────────────────

SV2025.tos: SV2025_atari.o backend_gemtos.o segline.o clipline.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > SV2025.sym

SV2025-ascii.tos: SV2025_atari.o backend_ascii_atari.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)

sv2025-sdl: SV2025_linux.o backend_sdl.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX) $(SDL_LIBS)

sv2025-ascii: SV2025_linux.o backend_ascii_linux.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX)
