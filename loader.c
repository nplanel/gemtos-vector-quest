#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>

#include "lz4Unpack.c"
#include "lz4_vquest.h"

uint8_t *gScreenBufferA;
uint8_t *gScreenBufferB;

#define SCREEN_BYTES_PER_ROW 160

static inline void backend_draw_star(uint16_t x, uint16_t y) {
    /* Write to plane 3 of both screen buffers so stars survive buffer swaps.
       4-plane interleaved: each 8-byte group is [plane0][plane1][plane2][plane3].
       Plane 3 word offset within group = +6. */
    uint16_t *a = (uint16_t *)((uint8_t *)gScreenBufferA
                  + y * SCREEN_BYTES_PER_ROW + ((uint16_t)(x >> 4) << 3) + 6);
    uint16_t *b = (uint16_t *)((uint8_t *)gScreenBufferB
                  + y * SCREEN_BYTES_PER_ROW + ((uint16_t)(x >> 4) << 3) + 6);
    uint16_t bit = (uint16_t)(0x8000u >> (x & 15u));
    *a |= bit;
    *b |= bit;
}

#include "stars.c"

static const uint16_t kGlowStar[16]  = {
    0x222, 0x333, 0x333, 0x444, 0x555, 0x555, 0x666, 0x666,
    0x666, 0x666, 0x555, 0x555, 0x444, 0x333, 0x333, 0x222
};

#include "snd_data.h"

#define LZ4_LODADER 1
#define ZIK 1

#define SND_ISR_ADDRESS          (volatile __uint8_t *)0xFFFFFA0FL
#define SND_END_OF_INTERRUPT     (~(1 << 5))
#define PSG_REGISTER_INDEX       (volatile __uint8_t *)0xFF8800
#define PSG_REGISTER_DATA        (volatile __uint8_t *)0xFF8802

#ifdef ZIK
static inline void write_psg(__uint8_t reg, __uint8_t val)
{
    (*PSG_REGISTER_INDEX) = reg;
    (*PSG_REGISTER_DATA)  = val;
}

uint8_t *zikData;
uint16_t zikFrame = 0;
uint16_t zikNbFrames;
volatile uint8_t oneloop = 0;
static void __attribute__((interrupt)) timera_interrupt(void) {
    uint8_t *frame = zikData + zikFrame;
    for (int i = 0; i < 14; i++) {
        write_psg(i, *frame);
        frame += zikNbFrames;
    }
    zikFrame++;
    if (zikFrame >= zikNbFrames) {
        zikFrame = 0;
        oneloop = 1;
    }
    uint16_t star  = kGlowStar [(zikFrame >> 2) & 15];
    Setcolor(8, star);

    *(SND_ISR_ADDRESS) &= SND_END_OF_INTERRUPT;
}
#endif

int main(int argc, char *argv[])
{
    // splash screen
    for (int i = 0; i < 16; i++) {
        if (i == 8)
            Setcolor(i, 0x222); // stars
        else
            Setcolor(i, 0x000);
    }
    uint8_t *phy = (uint8_t *)VQUEST_LOAD_ADDRESS - 32000UL;//Physbase();
    gScreenBufferA = phy;
    gScreenBufferB = phy;
    Setscreen(Logbase(), phy, 0);

#ifdef ZIK
    zikData = (uint8_t *)Malloc(4096);
    long int len = lz4FrameUnpack(zikData, kZikIntroLZ4);
    zikData += 0x3b;
    zikNbFrames = (len - 0x3b - 4) / 16;

    // init intro zik
    void snd_play_supervisor(void) {
        Jdisint(13);
        Xbtimer(0, 7, 246, timera_interrupt);
        Jenabint(13);
    }
    Supexec(snd_play_supervisor);
#endif

    stars_init();

#ifdef LZ4_LODADER
    int16_t f = Fopen("VQUEST.LZ4", 1);
    uint8_t *prg_buffer_lz4 = (uint8_t *)Malloc(VQUEST_LZ4_SIZE);
#ifdef DEBUG
    if (!prg_buffer_lz4) {
        (void)Cconws("Error allocating LZ4 buffer\r\n");
        return 1;
    }
#endif
    int32_t r = Fread(f, VQUEST_LZ4_SIZE, prg_buffer_lz4);
#ifdef DEBUG
    if (r != VQUEST_LZ4_SIZE) {
        (void)Cconws("Error reading VQUEST.LZ4\r\n");
        return 1;
    }
#endif

    BASEPAGE *bp = (uint8_t *)VQUEST_LOAD_ADDRESS;
    int32_t file_len = lz4FrameUnpack(bp, prg_buffer_lz4);

    // optional
    Mfree(prg_buffer_lz4);
    Fclose(f);
#else
    FILE *f = fopen("VQUEST", "rb");
    BASEPAGE *bp = (uint8_t *)(memtop-VQUEST_SIZE-0x10000);
    printf("Reading file into %p\r\n", bp);
    long int file_len = fread(bp, 1, VQUEST_SIZE, f);
    fclose(f);
#endif

    // execute the loaded program
    BASEPAGE *new = (BASEPAGE *)bp->p_lowtpa;
#ifdef DEBUG
    printf("Unpacked program: flen %ld TEXT=%ld bytes, DATA=%ld bytes, BSS=%ld bytes\r\n", file_len,
           bp->p_tlen, bp->p_dlen, bp->p_blen);
    printf("bp %p\r\n", new);
#endif
//    memmove(new, bp, file_len);
    bzero(new->p_bbase, new->p_blen); // Clear BSS

#ifdef ZIKCLEANUP
    void snd_silence(void) { write_psg(7, 0b00111111); }
    void snd_stop_supervisor(void) { Jdisint(13); snd_silence(); }
    Supexec(snd_stop_supervisor);
    Mfree(zikData);
#endif
#ifdef ZIK
    while (!oneloop) {  }
#endif
    // AO = 0 : Application gem, accessory otherwise
    __asm__ __volatile__ (
        "move.l %0, %%a0\n\t"
        "move.l %0, %%a6\n\t"
        "jmp    (%%a6)\n\t"
        :
        : "g" (new->p_tbase)
        : "a0", "a6"
    );
    return 0;
}

