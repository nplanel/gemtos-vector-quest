/* gen_snd.c — host-side build tool: bakes YM6 tracks into a C header.
 *
 *   gen_snd [--sfx] Name=path.ym [Name=path.ym ...] > header.h
 *
 * For each track it emits kZik<Name>Frames and kZik<Name>[], the register
 * columns R0..R13 of the interleaved (column-major) YM6 frame data —
 * data[reg * frames + frame], the layout YmTrack in atari_common.h reads.
 * Tracks after --sfx are sound effects, mixed over the theme by
 * backend_gemtos.c's snd_write_mixed: their trailing frames that enable no
 * channel are dropped, since such a frame hands every register back to the
 * theme and the effect may as well end there (Enmyhit: 384 frames, only the
 * first 36 audible).  An effect also gets three more rows after R13, the
 * mixing masks snd_write_mixed would otherwise derive from R7-R10 on every
 * tick (see sfx_masks).
 * Stripped at build time instead of at run time:
 *   - the header (song name/author/comment strings, variable length),
 *   - the R14/R15 columns (I/O ports: never written, see YM_SOUND_REGS),
 *   - the "End!" trailer.
 * R7 bit 6 (Port A = output, the floppy/RS-232 control lines) is set in every
 * frame here so the players never have to patch it per write.
 *
 * The data is embedded raw, not LZ4-packed: the shipped game image is itself
 * LZ4-packed by the loader's build, which compresses these columns about as
 * well, so packing them again only cost an unpack buffer and the depacker.
 *
 * Anything the players cannot handle (digidrums, non-interleaved data, a
 * frame count that disagrees with the file size) fails the build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define YM_REGS_IN_FILE 16
#define YM_REGS_KEPT    14   /* must equal YM_SOUND_REGS (atari_common.h) */
#define YM6_ATTR_INTERLEAVED 1u

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int fail(const char *path, const char *why) {
    fprintf(stderr, "gen_snd: %s: %s\n", path, why);
    return 1;
}

/* Which registers effect frame k takes over from the theme: every channel
 * the effect enables (tone or noise, R7 bits active-low) brings its period,
 * volume and R7 mixer bits, plus the noise period (R6) if one of its
 * channels uses noise and the envelope (R11-R13) if one is envelope-driven.
 * take: bit n = Rn; own: the R7 bits the effect owns. */
static void sfx_masks(const uint8_t *d, uint32_t frames, uint32_t k,
                      uint16_t *take, uint8_t *own)
{
    uint8_t r7 = d[7 * frames + k], mask;
    uint32_t c;
    *take = 0; *own = 0;
    for (c = 0, mask = 0x09; c < 3; c++, mask = (uint8_t)(mask << 1)) {
        if ((r7 & mask) == mask) continue;       /* channel unused */
        *own  |= mask;
        *take |= (uint16_t)((3u << (2 * c)) | (0x100u << c));
        if (!(r7 & mask & 0x38))        *take |= 1u << 6;
        if (d[(8 + c) * frames + k] & 0x10) *take |= 7u << 11;
    }
}

static int emit_track(const char *name, const char *path, int sfx)
{
    static uint8_t buf[256 * 1024];
    FILE *f = fopen(path, "rb");
    size_t len, off;
    uint32_t frames, kept, rows, r, i;
    int s;

    if (!f) return fail(path, "cannot open");
    len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (len < 34 || len == sizeof(buf)) return fail(path, "bad size");
    if (memcmp(buf, "YM6!LeOnArD!", 12) != 0) return fail(path, "not a YM6 file");
    frames = be32(buf + 12);
    if (!(be32(buf + 16) & YM6_ATTR_INTERLEAVED)) return fail(path, "not interleaved");
    if (be16(buf + 20) != 0) return fail(path, "has digidrums");
    if (frames == 0 || frames > 0xFFFF) return fail(path, "frame count out of range");
    off = 34 + be16(buf + 32);                 /* skip additional data */
    for (s = 0; s < 3; s++) {                  /* name, author, comment */
        while (off < len && buf[off]) off++;
        off++;
    }
    if (off + (size_t)frames * YM_REGS_IN_FILE + 4 != len ||
        memcmp(buf + len - 4, "End!", 4) != 0)
        return fail(path, "frame data does not match the header");

    for (i = 0; i < frames; i++)
        buf[off + 7 * frames + i] |= 0x40;     /* R7: Port A stays an output */

    kept = frames;
    if (sfx)   /* R7 bits 0-5 set: every tone and noise channel disabled */
        while (kept > 1 && (buf[off + 7 * frames + kept - 1] & 0x3f) == 0x3f)
            kept--;

    rows = YM_REGS_KEPT + (sfx ? 3 : 0);       /* + take R0-7, take R8-13, own */
    printf("#define kZik%sFrames %uu\n", name, (unsigned)kept);
    printf("static const uint8_t kZik%s[%u * %u] = {", name,
           (unsigned)rows, (unsigned)kept);
    for (r = 0; r < rows * kept; r++) {
        uint32_t row = r / kept, k = r % kept;
        uint8_t v;
        if (row < YM_REGS_KEPT) v = buf[off + row * frames + k];
        else {
            uint16_t take; uint8_t own;
            sfx_masks(buf + off, frames, k, &take, &own);
            v = row == YM_REGS_KEPT     ? (uint8_t)take
              : row == YM_REGS_KEPT + 1 ? (uint8_t)(take >> 8) : own;
        }
        printf("%s0x%02x,", r % 16 ? " " : "\n    ", v);
    }
    printf("\n};\n\n");
    return 0;
}

int main(int argc, char **argv)
{
    int i, sfx = 0;
    printf("/* generated by gen_snd — do not edit */\n");
    printf("#include <stdint.h>\n");
    printf("#define SND_DATA_REGS %d\n\n", YM_REGS_KEPT);
    for (i = 1; i < argc; i++) {
        char *eq;
        if (strcmp(argv[i], "--sfx") == 0) { sfx = 1; continue; }
        eq = strchr(argv[i], '=');
        if (!eq) return fail(argv[i], "expected Name=path.ym");
        *eq = 0;
        if (emit_track(argv[i], eq + 1, sfx)) return 1;
    }
    return 0;
}
