#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <mint/osbind.h>
#include <mint/sysvars.h>

#define INTERRUPTION_SERVICE_ADDRESS (volatile __uint8_t *)0xFFFFFA0FL
#define END_OF_INTERRUPT_TIMER_A  (~(1 << 5)) // reset bit 5

#define PSG_REGISTER_INDEX_ADDRESS (volatile __uint8_t *)0xFF8800
#define PSG_REGISTER_DATA_ADDRESS (volatile __uint8_t *)0xFF8802

unsigned int zikNbFrames;
unsigned int zikFrame;
unsigned char *zikData;

static inline void write_PSG(__uint8_t registerIndex, __uint8_t registerValue)
{
    (*PSG_REGISTER_INDEX_ADDRESS) = registerIndex;
    (*PSG_REGISTER_DATA_ADDRESS) = registerValue;
}

static void __attribute__((interrupt)) timera_interrupt()
{
    if (zikFrame >= zikNbFrames) {
        zikFrame = 0;
    } else {
        zikFrame++;
    }

    unsigned char *address = zikData + zikFrame;

    for (int i = 0; i < 14; i++) {
        write_PSG(i, *address);
        address += zikNbFrames;
    }

    *(INTERRUPTION_SERVICE_ADDRESS) &= END_OF_INTERRUPT_TIMER_A;
}

void soundOff()
{
    write_PSG(7, 0b00111111); // R7 : mixer, deactivate (1 !) all
}

static unsigned char originalKeyClick;

static void disableKeyClick() {
    originalKeyClick = *(conterm);
    *(conterm) = 0b11111110 & originalKeyClick;
}

static void restoreKeyClick() {
    *(conterm) = originalKeyClick;
}

void zik_init(void)
{
    FILE *fp = fopen("sound/ancool1.ym", "rb");
    fseek(fp,0,SEEK_END);
    unsigned fileSize=ftell(fp);
    zikData = malloc(fileSize);
    fseek(fp,0,SEEK_SET);
    fread(zikData, 1, fileSize, fp);
    fclose(fp);

    zikData += 4; // skip header YM3!
    zikNbFrames = (fileSize-4) / 14;
    zikFrame = 0;

    Supexec(disableKeyClick);
}

void zik_play(void)
{
    void play() {
        Jdisint(13);
        Xbtimer(0, 7, 246, timera_interrupt); // 50 Hz
        Jenabint(13);
    }
    Supexec(play);
}

void zik_stop(void)
{
    void stop() {
        Jdisint(13);
        soundOff();
    }
    Supexec(stop);
}

void zik_cleanup(void)
{
    Supexec(restoreKeyClick);
    free(zikData - 4);
}
