#include <stdint.h>

/* TOS Program Header structure (28 bytes) */
typedef struct {
    uint16_t ph_branch;    /* Branch to start (usually 0x601a) */
    uint32_t ph_tlen;      /* Length of TEXT segment */
    uint32_t ph_dlen;      /* Length of DATA segment */
    uint32_t ph_blen;      /* Length of BSS segment */
    uint32_t ph_slen;      /* Length of symbol table */
    uint32_t ph_res1;      /* Reserved */
    uint32_t ph_prgflags;  /* Program flags */
    uint16_t ph_absflag;   /* 0 = relocation needed, != 0 = absolute */
} __attribute__((packed)) PH;

#define PH_SIZE 28

int32_t load_and_exec(const uint8_t *prg_data, uint32_t prg_len)
{
    const PH *header = (const PH *)prg_data;
    uint32_t text_len, data_len, bss_len, sym_len;
    uint8_t *text_start;
    uint8_t *bss_start;
    const uint8_t *reloc_table;
    uint32_t reloc_offset;
    uint8_t *reloc_ptr;
    uint8_t reloc_byte;

    text_len = header->ph_tlen;
    data_len = header->ph_dlen;
    bss_len  = header->ph_blen;
    sym_len  = header->ph_slen;

    /* TEXT segment starts right after header */
    text_start = (uint8_t *)(prg_data + PH_SIZE);
    /* BSS starts after TEXT + DATA */
    bss_start = text_start + text_len + data_len;

    /* Perform relocation if needed */
    if (header->ph_absflag == 0) {
        /* Relocation table is after TEXT + DATA + symbols */
        reloc_table = prg_data + PH_SIZE + text_len + data_len + sym_len;

        /* First 4 bytes are the offset of first relocation */
        reloc_offset = ((uint32_t)reloc_table[0] << 24) |
                       ((uint32_t)reloc_table[1] << 16) |
                       ((uint32_t)reloc_table[2] << 8)  |
                       ((uint32_t)reloc_table[3]);

        if (reloc_offset != 0) {
            /* Point to first location to relocate */
            reloc_ptr = text_start + reloc_offset;

            /* Apply first relocation: add base address to the longword */
            *(uint32_t *)reloc_ptr += (uint32_t)text_start;
            reloc_table += 4;

            while ((reloc_byte = *reloc_table++) != 0) {
                if (reloc_byte == 1) {
                    /* Special case: add 254 to offset without relocating */
                    reloc_ptr += 254;
                } else {
                    /* Normal case: add byte value to offset and relocate */
                    reloc_ptr += reloc_byte;
                    *(uint32_t *)reloc_ptr += (uint32_t)text_start;
                }
            }
        }
    }

    if (bss_len > 0) {
        bzero(bss_start, bss_len);
    }

    /* Jump to program entry point (start of TEXT segment) */
    /* Using inline assembly to perform the jump */
    __asm__ __volatile__ (
        "move.l %0, %%a0\n\t"
        "jmp    (%%a0)\n\t"
        :
        : "g" (text_start)
        : "a0"
    );

    // Should never reach here
    return -1;
}
