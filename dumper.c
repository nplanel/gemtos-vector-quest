/* dumper.c — build-time tool (vquest.raw rule); never shipped.
 *
 * Pexec(3)-loads VQUEST.TOS (load, don't go: GEMDOS relocates the image for
 * its load address) and writes basepage+text+data to a file named VQUEST.
 * That raw image becomes vquest.lz4, which loader.c unpacks back to the very
 * same absolute address (VQUEST_LOAD_ADDRESS in the generated lz4_vquest.h)
 * before jumping in — so the image must land ABOVE the boot loader's runtime
 * footprint (loader text+bss + zik + lz4 read buffers ≈ 40KB above membot).
 *
 * The image address must NOT depend on this tool's own footprint: the boot
 * environment's TPA starts lower than hatari-prg-args' and the loader
 * Mallocs ~30KB of read buffers below the image.  We pad free RAM up to a
 * fixed IMAGE_BASE so Pexec(3) deterministically loads there — above the
 * loader's worst-case top (~180KB), low enough to leave the game headroom.
 *
 * This used to live inside the game (Getrez()==2 hack), dragging printf,
 * stdio and the soft-double library into the shipped binary.
 */
#include <stddef.h>
#include <stdint.h>
#include <mint/osbind.h>

#define IMAGE_BASE 0x40000L   /* 256KB: child TPA base = dumped basepage address */

int main(void)
{
    /* Pad free RAM so the next allocation — Pexec(3)'s child TPA — starts
     * exactly at IMAGE_BASE.  GEMDOS block addresses are exact (memory
     * descriptors live out-of-band).  Freed by GEMDOS at exit. */
    uint8_t *base = (uint8_t *)Malloc(16);
    long     pad  = IMAGE_BASE - (long)base;
    Mfree(base);
    if (pad <= 0) { (void)Cconws("dumper: free RAM above IMAGE_BASE\r\n"); return 1; }
    (void)Malloc(pad);

    BASEPAGE *bp = (BASEPAGE *)Pexec(3, "VQUEST.TOS", NULL, NULL);
    if ((long)bp <= 0) { (void)Cconws("dumper: Pexec failed\r\n");   return 1; }

    long size = (long)sizeof(BASEPAGE) + bp->p_tlen + bp->p_dlen;
    int16_t f = Fcreate("VQUEST", 0);
    if (f < 0)          { (void)Cconws("dumper: Fcreate failed\r\n"); return 1; }

    long w = Fwrite(f, size, bp);
    (void)Fclose(f);
    if (w != size)      { (void)Cconws("dumper: Fwrite failed\r\n");  return 1; }

    (void)Cconws("dumper: wrote VQUEST\r\n");
    return 0;
}
