CC_ATARI  = m68k-atari-mint-gcc
STRIP_ATARI  = m68k-atari-mint-strip
CC_LINUX  = gcc
VASM      = vasm

LIBCMINI_DIR ?= ../libcmini
LIBCMINI     = $(LIBCMINI_DIR)/build/mshort
CRT0         = $(LIBCMINI)/objs/crt0.o

VASMFLAGS = -Faout -quiet -x -m68000 -spaces -showopt

# ── Optimisation level — comment/uncomment to switch ───────────────────────────
OPT = -Ofast -DNDEBUG
#OPT = -Og

# ── Flags common to all targets ────────────────────────────────────────────────
CFLAGS_COMMON = -Wall -Wextra -Werror -g -std=gnu99

CFLAGS_ATARI  = $(OPT) $(CFLAGS_COMMON) -mshort -nostdlib -I$(LIBCMINI_DIR)/include -MMD -MP
CFLAGS_LOADER = -Os  $(CFLAGS_COMMON) -mshort -nostdlib -I$(LIBCMINI_DIR)/include -MMD -MP
CFLAGS_LINUX  = $(OPT) $(CFLAGS_COMMON) -fsanitize=address,undefined -MMD -MP

LDFLAGS_ATARI = -L$(LIBCMINI) -lcmini -lgcc -lcmini
LDFLAGS_LINUX = -fsanitize=address,undefined

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

# clipline.o intentionally not linked: ClipLine has no caller (≈1KB dead code).
OBJS_ASM = segline.o

all: vquest.tos vq-sdl vq-ascii vq-ascii.tos vq-bench.tos vquest.st

clean:
	rm -f vquest.raw *.o *.d *.tos *.st *.lz4 lz4_vquest.h *.sym vq-sdl vq-ascii vq-bench vq-bench.tos snd_data.h

vquest.st: loader.tos vquest.strip.tos vquest.lz4
	dd if=/dev/zero of=$@ bs=1k count=720
	mformat -a -f 720 -i $@ ::
	MTOOLS_NO_VFAT=1 mmd -i $@ ::AUTO
	MTOOLS_NO_VFAT=1 mcopy -i $@ -spmv vquest.lz4 ::VQUEST.LZ4
	MTOOLS_NO_VFAT=1 mcopy -i $@ -spmv $< ::AUTO/LOADER.PRG
	MTOOLS_NO_VFAT=1 mcopy -i $@ -spmv vquest.strip.tos ::VQUEST.PRG

.PHONY: run
run: vquest.tos
	hatari-prg-args -q --conout 2 --fast-boot true -- $<

.PHONY: floppy
floppy: vquest.st
	hatari --conout 2 --fast-boot true $<

.PHONY: bench
bench: vq-bench.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-boot true --benchmark --sound off --disable-video on -- $<

# ── 2-player race-mode testing ─────────────────────────────────────────────────
# Two FIFOs form a null-modem link between player slots A and B.
# Run any one A target and any one B target in two terminals; all pairings
# work: race-hatari-a + race-sdl-b, race-hatari-a + race-hatari-b, etc.
RACE_A2B = /tmp/vq-race-a2b
RACE_B2A = /tmp/vq-race-b2a

.PHONY: race-fifos
race-fifos:
	test -p $(RACE_A2B) || mkfifo $(RACE_A2B)
	test -p $(RACE_B2A) || mkfifo $(RACE_B2A)

# 3<>/4<> hold both FIFOs open read-write so hatari's blocking fopen()s
# succeed immediately regardless of which side starts first.
.PHONY: race-hatari-a race-hatari-b race-sdl-a race-sdl-b
race-hatari-a: vquest.tos race-fifos
	hatari-prg-args -q --conout 2 --fast-boot true --rs232-in $(RACE_B2A) --rs232-out $(RACE_A2B) -- $< 3<>$(RACE_A2B) 4<>$(RACE_B2A)

race-hatari-b: vquest.tos race-fifos
	hatari-prg-args -q --conout 2 --fast-boot true --rs232-in $(RACE_A2B) --rs232-out $(RACE_B2A) -- $< 3<>$(RACE_A2B) 4<>$(RACE_B2A)

race-sdl-a: vq-sdl race-fifos
	./vq-sdl 0 0 $(RACE_A2B) $(RACE_B2A)

race-sdl-b: vq-sdl race-fifos
	./vq-sdl 0 0 $(RACE_B2A) $(RACE_A2B)

# Text-backend race instances: autopiloted, frames stream to the terminal.
# VQ_FRAME_MS paces the free-running Linux build at ~50 Hz.
.PHONY: race-ascii-a race-ascii-b race-ascii-tos-a race-ascii-tos-b
race-ascii-a: vq-ascii race-fifos
	VQ_FRAME_MS=20 ./vq-ascii 0 0 $(RACE_A2B) $(RACE_B2A)

race-ascii-b: vq-ascii race-fifos
	VQ_FRAME_MS=20 ./vq-ascii 0 0 $(RACE_B2A) $(RACE_A2B)

race-ascii-tos-a: vq-ascii.tos race-fifos
	hatari-prg-args -q --conout 2 --fast-boot true --rs232-in $(RACE_B2A) --rs232-out $(RACE_A2B) -- $< 3<>$(RACE_A2B) 4<>$(RACE_B2A)

race-ascii-tos-b: vq-ascii.tos race-fifos
	hatari-prg-args -q --conout 2 --fast-boot true --rs232-in $(RACE_A2B) --rs232-out $(RACE_B2A) -- $< 3<>$(RACE_A2B) 4<>$(RACE_B2A)

.PHONY: test test-race
test: test-race
test-race: vq-ascii vq-ascii.tos
	./test_race.sh

# dumper.tos Pexec(3)-loads VQUEST.TOS under hatari and writes the relocated
# basepage+text+data image to "VQUEST" (see dumper.c).  Depends on vquest.tos:
# that is the file the emulated GEMDOS resolves "VQUEST.TOS" to (stripped and
# unstripped text+data are identical, so the image matches the shipped PRG).
vquest.raw: dumper.tos vquest.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-boot true --benchmark --sound off --disable-video on --memsize 1 -- $<
	mv VQUEST $@

dumper.tos: dumper.c
	$(CC_ATARI) $(CFLAGS_LOADER) -s $(CRT0) dumper.c -o $@ $(LDFLAGS_ATARI)

# ── Per-binary unity compilation ───────────────────────────────────────────────

loader.ym.lz4: sound/loader.ym
	lz4 -f -9 --no-frame-crc $< $@
	touch $@

# Tracks are embedded LZ4-compressed (~16:1 on YM register dumps) and unpacked
# into RAM by snd_setup(); kZik*RawSize sizes the unpack buffer.  The L suffix
# keeps the summed buffer size out of 16-bit int range under -mshort.
snd_data.h: sound/intro.ym sound/main.ym sound/fire.ym sound/gameover.ym sound/enmyhit.ym
	@{ \
	  echo "/* generated — do not edit. Regenerate with: make snd_data.h */"; \
	  for t in Intro:intro Main:main Fire:fire Gameover:gameover Enmyhit:enmyhit; do \
	    name=$${t%%:*}; ym=sound/$${t#*:}.ym; \
	    echo "#define kZik$${name}RawSize $$(stat -c%s $$ym)L"; \
	    lz4 -f -9 -q --no-frame-crc $$ym ymtmp; \
	    xxd -i ymtmp | sed -e 's/^unsigned /static const unsigned /' -e "s/ymtmp/kZik$${name}Lz4/g"; \
	  done; \
	  rm -f ymtmp; \
	} > $@

# Generated deps (snd_data.h) are listed explicitly for bootstrap on a clean
# build before any .d files exist; source deps are tracked by -MMD thereafter.
main_gemtos.o: main_gemtos.c snd_data.h
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

main_bench.o: main_bench.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

main_sdl.o: main_sdl.c
	$(CC_LINUX) $(CFLAGS_LINUX) $(SDL_CFLAGS) -c $< -o $@

main_ascii.o: main_ascii.c
	$(CC_LINUX) $(CFLAGS_LINUX) -c $< -o $@

main_ascii_tos.o: main_ascii_tos.c
	$(CC_ATARI) $(CFLAGS_ATARI) -c $< -o $@

# ── Assembly rules ─────────────────────────────────────────────────────────────

segline.o: segmented-line.git/segline.s
	$(VASM) $(VASMFLAGS) $< -o $@

clipline.o: segmented-line.git/clipline.s
	$(VASM) $(VASMFLAGS) $< -o $@

# ── Link targets ───────────────────────────────────────────────────────────────

# lz4_vquest.h is listed explicitly: it is generated, so loader.d does not
# exist on a clean build to pull it in via -include.
loader.tos: lz4_vquest.h
	$(CC_ATARI) $(CFLAGS_LOADER) -s $(CRT0) loader.c -o $@ $(LDFLAGS_ATARI)

lz4_vquest.h: vquest.raw vquest.lz4 loader.ym.lz4
	@{ \
	  echo "#ifndef LZ4_VQUEST_H"; \
	  echo "#define LZ4_VQUEST_H"; \
	  echo "#define VQUEST_LOAD_ADDRESS $$(od -An -N4 -tu4 --endian=big vquest.raw | tr -d ' ')"; \
	  echo "#define VQUEST_SIZE $$(stat -c%s vquest.raw)"; \
	  echo "#define VQUEST_LZ4_SIZE $$(stat -c%s vquest.lz4)"; \
	  xxd -i loader.ym.lz4 \
	    | sed -e 's/^unsigned /static const unsigned /' -e 's/loader_ym_lz4\b/kZikIntroLZ4/g'; \
	  echo "#endif"; \
	} > $@

vquest.lz4: vquest.raw
	lz4 -f -9 --no-frame-crc $< $@
	touch $@

vquest.strip.tos: vquest.tos
	m68k-atari-mint-strip -o $@ $<

vquest.tos: main_gemtos.o $(OBJS_ASM)
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

vq-bench.tos: main_bench.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

vq-sdl: main_sdl.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX) $(SDL_LIBS)

vq-ascii: main_ascii.o
	$(CC_LINUX) $^ -o $@ $(LDFLAGS_LINUX)

vq-ascii.tos: main_ascii_tos.o
	$(CC_ATARI) -mshort -nostdlib $(CRT0) $^ -o $@ $(LDFLAGS_ATARI)
	gst2ascii $@ > $(@:.tos=.sym)

-include $(wildcard *.d)
