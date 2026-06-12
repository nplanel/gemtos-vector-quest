/* dumper.c — build-time tool (vquest.raw rule); never shipped.
 *
 * Pexec(3)-loads VQUEST.TOS (load, don't go: GEMDOS relocates the image for
 * its load address) and writes basepage+text+data to a file named VQUEST.
 * That raw image becomes vquest.lz4, which loader.c unpacks back to the very
 * same absolute address (VQUEST_LOAD_ADDRESS in the generated lz4_vquest.h)
 * before jumping in — so the image must land ABOVE the boot loader's runtime
 * footprint (loader text+bss + zik + lz4 read buffers ≈ 40KB above membot).
 *
 * Under hatari-prg-args the TPA already starts around 140KB; the Malloc
 * cushion adds margin against membot differences across TOS versions.  Keep
 * it small: every byte of cushion is dead RAM for the unpacked game.
 *
 * This used to live inside the game (Getrez()==2 hack), dragging printf,
 * stdio and the soft-double library into the shipped binary.
 */
#include <stddef.h>
#include <stdint.h>
#include <mint/osbind.h>

#define CUSHION 32768L

int main(void)
{
    (void)Malloc(CUSHION);   /* held until exit; GEMDOS frees it by owner */

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
