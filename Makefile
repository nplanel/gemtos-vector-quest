

all: SV2025.tos

clean:
	rm -f *.o *.tos *.sym

SV2025.tos: SV2025.c
	vasm -Faout -quiet -x -m68000 -spaces -showopt segmented-line.git/segline.s -o segline.o
	vasm -Faout -quiet -x -m68000 -spaces -showopt segmented-line.git/clipline.s -o clipline.o
	m68k-atari-mint-gcc -O2 -Wall -Wextra -g -c -std=gnu99 SV2025.c -o SV2025.o
	m68k-atari-mint-gcc ./SV2025.o ./segline.o ./clipline.o -o SV2025.tos -lm -Wl,--traditional-format 
	gst2ascii SV2025.tos > SV2025.sym
