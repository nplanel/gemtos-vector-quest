
CC      = m68k-atari-mint-gcc
VASM    = vasm

VASMFLAGS = -Faout -quiet -x -m68000 -spaces -showopt
CFLAGS    = -O2 -Wall -Wextra -Werror -g -std=gnu99
LDFLAGS   = -lm -Wl,--traditional-format

all: SV2025.tos

clean:
	rm -f *.o *.tos *.sym

segline.o: segmented-line.git/segline.s
	$(VASM) $(VASMFLAGS) $< -o $@

clipline.o: segmented-line.git/clipline.s
	$(VASM) $(VASMFLAGS) $< -o $@

SV2025.o: SV2025.c
	$(CC) $(CFLAGS) -c $< -o $@

SV2025.tos: SV2025.o segline.o clipline.o
	$(CC) $^ -o $@ $(LDFLAGS)
	gst2ascii $@ > SV2025.sym
