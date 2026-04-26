#include <stdint.h>
#include <stdio.h>
#include <mint/osbind.h>

#include "tosloader.c"
#include "lz4Unpack.c"

#include "lz4_vquest.h"

int main(void)
{
    int32_t file_len;
    uint8_t *prg_buffer;
    uint8_t *prg_buffer_lz4;

    // splash screen here
    // init intro zik here

    int16_t f = Fopen("VQUEST.LZ4", 0);
    prg_buffer_lz4 = (uint8_t *)Malloc(VQUEST_LZ4_SIZE);
    Fread(f, VQUEST_LZ4_SIZE, prg_buffer_lz4);
    prg_buffer = (uint8_t *)Malloc(VQUEST_SIZE);
    file_len = lz4FrameUnpack(prg_buffer, prg_buffer_lz4);
    // optional
    Mfree(prg_buffer_lz4);
    Fclose(f);

    load_and_exec(prg_buffer, file_len);
    // Should never reach here
    return 0;
}

