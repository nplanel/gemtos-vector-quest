#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>
#include <mint/basepage.h>

/* Depacker at O3 in the otherwise -Os loader: boot time is 84% floppy I/O /
 * 16% decompress, and O3 cuts the unpack of VQUEST.LZ4 from 325 ms to
 * 205 ms for +229 B of loader (~18 ms extra load) — measured on hatari with
 * hz200 stamps around Fread and lz4FrameUnpack. */
#pragma GCC push_options
#pragma GCC optimize("O3")
#include "lz4Unpack.c"
#pragma GCC pop_options
#include "lz4_vquest.h"
#include "atari_common.h"

screen_t *gScreenBufferA;
screen_t *gScreenBufferB;

static inline void backend_draw_star(uint16_t x, uint16_t y) {
    atari_draw_star(gScreenBufferA, x, y);
}

#include "stars.c"

#define ZIK 1

#ifdef ZIK
static YmTrack zikIntro;

static void zik_vbl(void) {
    uint8_t buf[14];
    ym_fill_frame(&zikIntro, buf, 14);
    ym_write_regs(buf, 14);
    ym_advance(&zikIntro);
    PALETTE[8] = kGlowStar[(zikIntro.frame >> 2) & 15];
}
#endif

/* Apply the GEMDOS relocation table that follows text+data: a longword giving
 * the first fixup's offset (0 = no relocation), then bytes — 0 ends the table,
 * 1 advances 254 without fixing up, anything else advances by that many bytes
 * and fixes up the longword there. */
static void relocate(char *text, long tdlen)
{
    uint8_t *p = (uint8_t *)text + tdlen;
    uint32_t first = *(uint32_t *)p;
    uint32_t delta = (uint32_t)text;    /* image is linked at 0 */
    p += 4;
    if (!first) return;
    char *fix = text + first;
    *(uint32_t *)fix += delta;
    for (;;) {
        uint8_t b = *p++;
        if (b == 0) break;
        if (b == 1) { fix += 254; continue; }
        fix += b;
        *(uint32_t *)fix += delta;
    }
}

int main(int argc, char *argv[])
{
    const char *errmsg;
    int16_t   f      = -1;
    uint8_t  *packed = NULL;
    uint8_t  *zikBuf = NULL;
    BASEPAGE *bp     = NULL;
    long      rc;

    /* Snapshot screen state before wrecking it: rez first, because a rez
     * change resets the palette (restore order below mirrors this). */
    int16_t  savedRez = Getrez();
    uint16_t savedPal[16];
    for (int i = 0; i < 16; i++) savedPal[i] = (uint16_t)Setcolor(i, -1);

    // splash screen
    for (int i = 0; i < 16; i++) {
        if (i == 8)
            (void)Setcolor(i, 0x222); // stars
        else
            (void)Setcolor(i, 0x000);
    }
    screen_t *phy = Physbase();
    gScreenBufferA = phy;
    gScreenBufferB = phy;
    Setscreen(Logbase(), phy, 0);

    Supexec(snd_disable_key_click);

#ifdef ZIK
    zikBuf = (uint8_t *)Malloc(7168);
    long int len = lz4FrameUnpack(zikBuf, kZikIntroLZ4);
    zikIntro.data     = zikBuf + 0x3b;
    zikIntro.nbFrames = (uint16_t)((len - 0x3b - 4) / 16);
    zikIntro.frame    = 0;

    void snd_play_supervisor(void) {
        void (**q)(void) = (void(**)(void))*_vblqueue;
        for (short i = 0; i < *nvbls; i++) {
            if (!q[i]) { q[i] = zik_vbl; break; }
        }
    }
    Supexec(snd_play_supervisor);

    void snd_silence(void) { write_psg(7, 0b00111111); }
    void snd_stop_supervisor(void) {
        void (**q)(void) = (void(**)(void))*_vblqueue;
        for (short i = 0; i < *nvbls; i++) {
            if (q[i] == zik_vbl) { q[i] = NULL; break; }
        }
        snd_silence();
    }
#endif

    stars_init();

    /* Locate VQUEST.LZ4: argv[1] (desktop document-click via the .LZ4
     * association), else the floppy root relative to CWD (AUTO-folder boot,
     * or a document click under TOS 1.x), else the floppy root by absolute
     * path (double-clicking AUTO\VQUEST.PRG, CWD = \AUTO). Read-only open:
     * mode 1 fails on a write-protected disk. */
    if (argc > 1)  f = Fopen(argv[1], 0);
    if (f < 0)     f = Fopen("VQUEST.LZ4", 0);
    if (f < 0)     f = Fopen("\\VQUEST.LZ4", 0);
    if (f < 0) { errmsg = "Cannot open VQUEST.LZ4\r\n"; goto fail; }

    /* Read the compressed image before Pexec(5): mode 5 hands the child ALL
     * remaining free memory, so nothing can be Malloc'd afterwards. */
    packed = (uint8_t *)Malloc(VQUEST_LZ4_SIZE);
    if (!packed) { errmsg = "Out of memory\r\n"; goto fail; }
    if (Fread(f, VQUEST_LZ4_SIZE, packed) != VQUEST_LZ4_SIZE) {
        errmsg = "Cannot read VQUEST.LZ4\r\n"; goto fail;
    }
    (void)Fclose(f);
    f = -1;

    /* Pexec(5): create a basepage owning all free memory, nothing loaded.
     * The tail is a Pascal string and MUST stay empty: the game parses its
     * own argv (vquest.c:275-282 — frame limits, serial ports, "nobot"), and
     * our argv[1] is the .LZ4 path, which would otherwise land in argv[1]
     * there. */
    bp = (BASEPAGE *)Pexec(5, NULL, "", NULL);
    if ((long)bp <= 0) { errmsg = "Pexec(5) failed\r\n"; bp = NULL; goto fail; }

    if (bp->p_hitpa - bp->p_lowtpa < 256L + VQUEST_TEXT_SIZE + VQUEST_DATA_SIZE + VQUEST_BSS_SIZE) {
        errmsg = "Not enough memory\r\n"; goto fail;
    }

    bp->p_tbase = (char *)bp + 256;
    bp->p_tlen  = VQUEST_TEXT_SIZE;
    bp->p_dbase = bp->p_tbase + VQUEST_TEXT_SIZE;
    bp->p_dlen  = VQUEST_DATA_SIZE;
    bp->p_bbase = bp->p_dbase + VQUEST_DATA_SIZE;
    bp->p_blen  = VQUEST_BSS_SIZE;

    (void)lz4FrameUnpack((uint8_t *)bp->p_tbase, packed);
    relocate(bp->p_tbase, VQUEST_TEXT_SIZE + VQUEST_DATA_SIZE);
    bzero(bp->p_bbase, bp->p_blen);   /* AFTER relocate: the fixup table lands
                                       * at the start of the BSS area */
    Mfree(packed);
    packed = NULL;

#ifdef ZIK
    Supexec(snd_stop_supervisor);
    Mfree(zikBuf);
    zikBuf = NULL;
#endif

    rc = Pexec(4, NULL, (void *)bp, NULL);   /* returns when the game exits */
    Mfree(bp->p_env);
    Mfree(bp);

    Setscreen((void *)-1L, (void *)-1L, savedRez);
    for (int i = 0; i < 16; i++) (void)Setcolor(i, savedPal[i]);
    return (int)rc;

fail:
    /* Unhook the music VBL FIRST — the OS would keep calling zik_vbl in freed
     * memory every 50th of a second otherwise — then free whatever was
     * allocated, restore the screen, and report. */
#ifdef ZIK
    Supexec(snd_stop_supervisor);
    Mfree(zikBuf);
#endif
    if (bp)     { Mfree(bp->p_env); Mfree(bp); }
    if (packed) Mfree(packed);
    if (f >= 0) Fclose(f);
    Setscreen((void *)-1L, (void *)-1L, savedRez);
    for (int i = 0; i < 16; i++) (void)Setcolor(i, savedPal[i]);
    (void)Setcolor(15, 0xFFF);   /* console text colour was blacked out above */
    (void)Cconws(errmsg);
    (void)Cconin();              /* let the user read it before the desktop repaints */
    return 1;
}
