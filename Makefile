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

OBJS_ASM = segline.o clipline.o

all: vquest.tos vq-sdl vq-ascii vq-bench.tos vquest.st

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
	hatari $<

.PHONY: bench
bench: vq-bench.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --conout 2 --fast-forward true --fast-boot true -- $<

# Run under mono emulation: the program detects Getrez()==2 and dumps its
# relocated image to a file named "VQUEST" (hardcoded in backend_gemtos.c).
vquest.raw: vquest.strip.tos
	SDL_VIDEODRIVER=dummy hatari-prg-args -q --mono --conout 2 --fast-forward true --fast-boot true --memsize 1 -- $<
	mv VQUEST $@

# ── Per-binary unity compilation ───────────────────────────────────────────────

intro.ym.lz4: sound/intro.ym
	lz4 -f -9 --no-frame-crc $< $@
	touch $@

snd_data.h: sound/intro.ym sound/main.ym sound/fire.ym sound/gameover.ym sound/enmyhit.ym
	echo "/* generated — do not edit. Regenerate with: make snd_data.h */" > $@
	(cd sound && xxd -i intro.ym)    | sed -e 's/^unsigned /static const unsigned /' -e 's/intro_ym\b/kZikIntro/g'       >> $@
	(cd sound && xxd -i main.ym)     | sed -e 's/^unsigned /static const unsigned /' -e 's/main_ym\b/kZikMain/g'         >> $@
	(cd sound && xxd -i fire.ym)     | sed -e 's/^unsigned /static const unsigned /' -e 's/fire_ym\b/kZikFire/g'         >> $@
	(cd sound && xxd -i gameover.ym) | sed -e 's/^unsigned /static const unsigned /' -e 's/gameover_ym\b/kZikGameover/g' >> $@
	(cd sound && xxd -i enmyhit.ym)  | sed -e 's/^unsigned /static const unsigned /' -e 's/enmyhit_ym\b/kZikEnmyhit/g'  >> $@

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

lz4_vquest.h: vquest.raw vquest.lz4 intro.ym.lz4
	@{ \
	  echo "#ifndef LZ4_VQUEST_H"; \
	  echo "#define LZ4_VQUEST_H"; \
	  echo "#define VQUEST_LOAD_ADDRESS $$(od -An -N4 -tu4 --endian=big vquest.raw | tr -d ' ')"; \
	  echo "#define VQUEST_SIZE $$(stat -c%s vquest.raw)"; \
	  echo "#define VQUEST_LZ4_SIZE $$(stat -c%s vquest.lz4)"; \
	  xxd -i intro.ym.lz4 \
	    | sed -e 's/^unsigned /static const unsigned /' -e 's/intro_ym_lz4\b/kZikIntroLZ4/g'; \
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

-include $(wildcard *.d)
