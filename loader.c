#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>

#include "lz4Unpack.c"
#include "lz4_vquest.h"
#include "atari_common.h"

uint8_t *gScreenBufferA;
uint8_t *gScreenBufferB;

static inline void backend_draw_star(uint16_t x, uint16_t y) {
    atari_draw_star(gScreenBufferA, gScreenBufferB, x, y);
}

#include "stars.c"

#define LZ4_LODADER 1
//#define ZIK 1

#ifdef ZIK
static YmTrack zikIntro;
static volatile uint8_t oneloop = 0;

static void __attribute__((interrupt)) timera_interrupt(void) {
    uint8_t buf[14];
    ym_fill_frame(&zikIntro, buf, 14);
    ym_write_regs(buf, 14);
    if (ym_advance(&zikIntro)) oneloop = 1;
    (void)Setcolor(8, kGlowStar[(zikIntro.frame >> 2) & 15]);
    *(SND_ISR_ADDRESS) &= SND_END_OF_INTERRUPT;
}
#endif

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    // splash screen
    for (int i = 0; i < 16; i++) {
        if (i == 8)
            (void)Setcolor(i, 0x222); // stars
        else
            (void)Setcolor(i, 0x000);
    }
    uint8_t *phy = Physbase();//(uint8_t *)(((uintptr_t)VQUEST_LOAD_ADDRESS - 32256UL) & ~0xFFUL);
    gScreenBufferA = phy;
    gScreenBufferB = phy;
    Setscreen(Logbase(), phy, 0);

    Supexec(snd_disable_key_click);
#ifdef ZIK
    uint8_t *zikBuf = (uint8_t *)Malloc(7168);
    long int len = lz4FrameUnpack(zikBuf, kZikIntroLZ4);
    zikIntro.data     = zikBuf + 0x3b;
    zikIntro.nbFrames = (uint16_t)((len - 0x3b - 4) / 16);
    zikIntro.frame    = 0;

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
#else
    (void)r;
#endif

    uint8_t *unpack_dst = (uint8_t *)VQUEST_LOAD_ADDRESS;
    (void)lz4FrameUnpack(unpack_dst, prg_buffer_lz4);
    BASEPAGE *bp = (BASEPAGE *)unpack_dst;

    // optional
    Mfree(prg_buffer_lz4);
    Fclose(f);
#else
    FILE *f = fopen("VQUEST", "rb");
    BASEPAGE *bp = (uint8_t *)(memtop-VQUEST_SIZE-0x10000);
    printf("Reading file into %p\r\n", bp);
    long int file_len = fread(bp, 1, VQUEST_SIZE, f); (void)file_len;
    fclose(f);
#endif

    // execute the loaded program
    BASEPAGE *new = (BASEPAGE *)bp->p_lowtpa;
#ifdef DEBUG
    printf("Unpacked program: TEXT=%ld bytes, DATA=%ld bytes, BSS=%ld bytes\r\n",
           bp->p_tlen, bp->p_dlen, bp->p_blen);
    printf("bp %p\r\n", new);
#endif
//    memmove(new, bp, file_len);
    bzero(new->p_bbase, new->p_blen); // Clear BSS

#ifdef ZIK
    while (!oneloop) {  }
#endif
#ifdef ZIK
    void snd_silence(void) { write_psg(7, 0b00111111); }
    void snd_stop_supervisor(void) { Jdisint(13); snd_silence(); }
    Supexec(snd_stop_supervisor);
    Mfree(zikBuf);
#endif

    BASEPAGE *run;
    void getrun() {
        OSHEADER *O = *((OSHEADER **)(0x4f2L));
        O = O->os_beg;
        run = ((BASEPAGE**)O->p_run)[0];
    }
    Supexec(getrun);

    new->p_parent = run->p_parent;
    new->p_lowtpa = (char*)new;
    new->p_hitpa = run->p_hitpa;
    new->p_dta = run->p_dta;
    new->p_env = run->p_env;
    new->p_reserved = run->p_reserved;
    memcpy(new->p_undef, run->p_undef, sizeof(new->p_undef));
    memcpy(new->p_cmdlin, run->p_cmdlin, sizeof(new->p_cmdlin));
#if DEBUG
    printf("dta %p\r\n", new->p_dta);
    printf("env %p\r\n", new->p_env);
    printf("run %p\r\n", new->p_parent);
    printf("new->p_cmdlin '%.*s'\n",127, new->p_cmdlin);
#endif
    Pexec(4,"",new,"");
    return 0;
}
