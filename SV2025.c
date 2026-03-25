#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <osbind.h>
#include <mintbind.h>
#include <math.h>

#define ST_LOW_REZ_MODE 0
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 200
#define SCREEN_PLANES 4
#define SCREEN_BYTES_PER_ROW (SCREEN_WIDTH / 8 * SCREEN_PLANES) // 160
#define SCREEN_SIZE_BYTES (SCREEN_BYTES_PER_ROW * SCREEN_HEIGHT)

#define INT_ROTATION_SPEED ((short)(0.1f * (1 << 10))) // Fixed-point representation

// Perspective/Model Constants
#define LOGO_SCALE (3.0f/230.0f)
#define PERSPECTIVE_FACTOR_F 180.0f //  projection calc
#define OBJECT_Z_OFFSET_F 800.0f

#define COLOR_BACKGROUND 0
#define COLOR_FOREGROUND 1

typedef struct {
    float x, y, z;
} Point3DFloat;

typedef struct {
    short x, y, z;
} Point3DInt;

typedef struct {
    long x, y, z;
} Point3DLong;

typedef struct {
    short x, y;
} Point2D;



// from asm
void SegmentedLineSetup(void);
void SegmentedLine(unsigned short x0, unsigned short y0, unsigned short x1, unsigned short y1, void *buffer);
typedef struct {
    Point2D p0;
    Point2D p1;
} Line;
void SegmentedMultiLine(Line *lines, void *buffer);



static void* gScreenBufferA;
static void* gScreenBufferB;
static void* gActiveBuffer;
static unsigned char* gDrawingBuffer;

int gOriginalRez = -1;
unsigned short gOriginalPalette[16];

#define FP_SHIFT 10
#define FP_ONE (1 << FP_SHIFT)
#define FP_16 (16 * FP_ONE)

static inline double fixed_fp(short y_fp) {
    return (double)y_fp / FP_ONE;
}

#define LUT_SIZE 2048

short sinLUT[LUT_SIZE];
short cosLUT[LUT_SIZE];
short logLUT[LUT_SIZE];
short expLUT[LUT_SIZE * 2]; // Extra range for exp

void saveLUTs() {
    FILE *fp = fopen("luts", "w");
    fwrite(sinLUT, sizeof(sinLUT), 1, fp);
    fwrite(cosLUT, sizeof(cosLUT), 1, fp);
    fwrite(logLUT, sizeof(logLUT), 1, fp);
    fwrite(expLUT, sizeof(expLUT), 1, fp);
    fclose(fp);
}

void loadLUTs() {
    FILE *fp = fopen("luts", "r");
    fread(sinLUT, sizeof(sinLUT), 1, fp);
    fread(cosLUT, sizeof(cosLUT), 1, fp);
    fread(logLUT, sizeof(logLUT), 1, fp);
    fread(expLUT, sizeof(expLUT), 1, fp);
    fclose(fp);
}



void initLUTs() {
#if 0
    for (int i = 0; i < LUT_SIZE; i++) {
        float angle = 2 * M_PI * i / LUT_SIZE;
        sinLUT[i] = (short)(sin(angle) * FP_ONE);
        cosLUT[i] = (short)(cos(angle) * FP_ONE);
    }
    for (int i = 0; i < LUT_SIZE; i++) {
        float x = 1.0f * i / (LUT_SIZE/4); // 0 to 4
        logLUT[i] = (short)(log(x + 0.01f) * FP_ONE); // Avoid log(0)
    }
    for (int i = 0; i < LUT_SIZE * 2; i++) {
        float x = -16.0f + (32.0f * i / (LUT_SIZE * 2)); // -16 to 16
        float expVal = exp(x);
        // Clamp to prevent overflow
        if (expVal > 32767.0f / FP_ONE) expVal = 32767.0f / FP_ONE;
        if (expVal < -32768.0f / FP_ONE) expVal = -32768.0f / FP_ONE;
        expLUT[i] = (short)(expVal * FP_ONE);
    }
    saveLUTs();
#else
    loadLUTs();
#endif
}

static inline short fastSin(short angle) {
    short index = angle;
//    short index = (angle * LUT_SIZE) / (2 * FP_ONE * 31415 / 10000);
//    index = index % LUT_SIZE;
//    if (index < 0) index += LUT_SIZE;
    return sinLUT[index&(LUT_SIZE-1)];
}

static inline short fastCos(short angle) {
    short index = angle;
//    short index = (angle * LUT_SIZE) / (2 * FP_ONE * 31415 / 10000);
//    index = index % LUT_SIZE;
//    if (index < 0) index += LUT_SIZE;
    return cosLUT[index&(LUT_SIZE-1)];
}

static inline short fastLog(short x) {
    short index = (x * (LUT_SIZE/4)) >> FP_SHIFT;
    return logLUT[index&(LUT_SIZE-1)];
}

static inline short fastExp(short x) {
    short index = ((x + (16 << FP_SHIFT)) * (LUT_SIZE*2)) / (32 << FP_SHIFT);
    return expLUT[index&((LUT_SIZE*2)-1)];
}

static inline short mulViaLogExp(short a, short b)  {
    short aa = (a<0) ? -a:a;
    short bb = (b<0) ? -b:b;
    short r = fastExp(fastLog(aa) + fastLog(bb));
    if (((a!=aa) ^ (b!=bb))) {
        r = -r;
    }
    return r;
}

static inline short mulViaLogExpLegacy(short a, short b)  {
    if (a < 0 && b > 0) {
        return -fastExp(fastLog(-a) + fastLog(b));
    }
    if (a > 0 && b < 0) {
        return -fastExp(fastLog(a) + fastLog(-b));
    }
    if (a < 0 && b < 0) {
        return fastExp(fastLog(-a) + fastLog(-b));
    }
    return fastExp(fastLog(a) + fastLog(b));
}

short fixedMul(short a, short b) {
    return mulViaLogExp(a, b);
    //return (int)(a * b) >> FIXED_SHIFT;
}

#include "SV2025.h"

#define NUM_VERTICES (sizeof(sv2025Vertices) / sizeof(sv2025Vertices[0]))
Point3DLong gVerticesLongScale[NUM_VERTICES];
#define NUM_EDGES (sizeof(sv2025Edges) / sizeof(sv2025Edges[0]))

void model_scale() {
    unsigned i;
    for(i=0;i<NUM_VERTICES;i++) {
    	gVerticesLongScale[i].x = (long)((sv2025Vertices[i].x * LOGO_SCALE) * FP_ONE);
    	gVerticesLongScale[i].y = (long)((sv2025Vertices[i].y * LOGO_SCALE) * FP_ONE);
    	gVerticesLongScale[i].z = (long)((sv2025Vertices[i].z * LOGO_SCALE) * FP_ONE);
    }
}

int init_system() {
    short i;
    gOriginalRez = Getrez();
    if (gOriginalRez < 0) return 0;

    for (i = 0; i < 16; ++i) {
        gOriginalPalette[i] = Setcolor(i, -1);
    }

    void* raw_buffer = (void*)Malloc((SCREEN_SIZE_BYTES * 2) + 256);
    if (!raw_buffer) {
    	return 0;
    }
    gScreenBufferA = (void*)(((long)raw_buffer + 255L) & ~255L);
    gScreenBufferB = (void*)((long)gScreenBufferA + SCREEN_SIZE_BYTES);

    Setscreen(gScreenBufferA, gScreenBufferA, ST_LOW_REZ_MODE);
    gActiveBuffer = gScreenBufferA;
    gDrawingBuffer = gScreenBufferB;


    Setcolor(COLOR_BACKGROUND, 0x007);
    Setcolor(COLOR_FOREGROUND, 0x700);
    Setcolor(COLOR_FOREGROUND+1, 0x707);
    Setcolor(COLOR_FOREGROUND+2, 0x770);
    Setcolor(COLOR_FOREGROUND+3, 0x070);
    Setcolor(COLOR_FOREGROUND+4, 0x077);
    Setcolor(COLOR_FOREGROUND+5, 0x700);


    Cursconf(0, 0);

    memset(gScreenBufferA, 0, SCREEN_SIZE_BYTES);
    memset(gScreenBufferB, 0, SCREEN_SIZE_BYTES);

    return 1;
}

void restore_system() {
    short i;
    Cursconf(1, 0);

    for (i = 0; i < 16; ++i) {
        Setcolor(i, gOriginalPalette[i]);
    }
    if (gOriginalRez != -1) {
        Setscreen(-1L, -1L, gOriginalRez);
    }
}

static inline void plotsafe(int x, int y, unsigned short color_index, void* target_buffer) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT || !target_buffer) {
        return;
    }

    register unsigned int offset = ((unsigned int)y * (SCREEN_BYTES_PER_ROW)) + ((x & ~0x000f)>>1);
    register unsigned short bit_mask = 0x8000 >> (x & 0x000f);
    register unsigned char* base_addr = (unsigned char*)target_buffer;
    register unsigned short* p0 = (unsigned short*)&base_addr[offset];

    if (color_index & 1) p0[0] |= bit_mask; else p0[0] &= ~bit_mask;
    if (color_index & 2) p0[1] |= bit_mask; else p0[1] &= ~bit_mask;
    if (color_index & 4) p0[2] |= bit_mask; else p0[2] &= ~bit_mask;
    if (color_index & 8) p0[3] |= bit_mask; else p0[3] &= ~bit_mask;
}


static inline void plot1bp(unsigned short x, unsigned short y, unsigned short color_index __attribute__((unused)), void* buffer) {
static const unsigned short masktbl[] = {
    0x8000,0x4000,0x2000,0x1000,0x0800,0x0400,0x0200,0x0100,
    0x0080,0x0040,0x0020,0x0010,0x0008,0x0004,0x0002,0x0001,
};
static const unsigned short bytesperrow[] = {0,160,320,480,640,800,960,1120,1280,1440,1600,1760,1920,2080,2240,2400,2560,2720,2880,3040,3200,3360,3520,3680,3840,4000,4160,4320,4480,4640,4800,4960,5120,5280,5440,5600,5760,5920,6080,6240,6400,6560,6720,6880,7040,7200,7360,7520,7680,7840,8000,8160,8320,8480,8640,8800,8960,9120,9280,9440,9600,9760,9920,10080,10240,10400,10560,10720,10880,11040,11200,11360,11520,11680,11840,12000,12160,12320,12480,12640,12800,12960,13120,13280,13440,13600,13760,13920,14080,14240,14400,14560,14720,14880,15040,15200,15360,15520,15680,15840,16000,16160,16320,16480,16640,16800,16960,17120,17280,17440,17600,17760,17920,18080,18240,18400,18560,18720,18880,19040,19200,19360,19520,19680,19840,20000,20160,20320,20480,20640,20800,20960,21120,21280,21440,21600,21760,21920,22080,22240,22400,22560,22720,22880,23040,23200,23360,23520,23680,23840,24000,24160,24320,24480,24640,24800,24960,25120,25280,25440,25600,25760,25920,26080,26240,26400,26560,26720,26880,27040,27200,27360,27520,27680,27840,28000,28160,28320,28480,28640,28800,28960,29120,29280,29440,29600,29760,29920,30080,30240,30400,30560,30720,30880,31040,31200,31360,31520,31680,31840};

 register unsigned short offset = bytesperrow[y] + ((x & ~0x000f)>>1);
    register unsigned short bit_mask = masktbl[x & 0x000f]; //0x8000 >> (x & 0x000f);
    register unsigned char* base_addr = (unsigned char*)buffer;
    register unsigned short* p0 = (unsigned short*)&base_addr[offset];

    p0[0] |= bit_mask;
//    if (color_index & 1) p0[0] |= bit_mask; else p0[0] &= ~bit_mask;
//    if (color_index & 2) p0[1] |= bit_mask; else p0[1] &= ~bit_mask;
//    if (color_index & 4) p0[2] |= bit_mask; else p0[2] &= ~bit_mask;
//    if (color_index & 8) p0[3] |= bit_mask; else p0[3] &= ~bit_mask;
}

static inline void draw_line(short x1, short y1, short x2, short y2, unsigned short color_index, void* buffer) {
    register short dx = abs(x2 - x1);
    register short dy = -abs(y2 - y1);
    register short sx = (x1 < x2) ? 1 : -1;
    register short sy = (y1 < y2) ? 1 : -1;
    register short err = dx + dy;
    register short e2;

    short i;
    for (i=0;i<378;i++) {
        plot1bp(x1, y1, color_index, buffer);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

static inline Point3DInt rotate(unsigned i, short angleY, short angleX) {
    Point3DInt p_out;
    long x, y, z;
    long temp_x, temp_y, temp_z;

    const Point3DLong *p_in = &gVerticesLongScale[i];
    x = p_in->x;
    y = p_in->y;
    z = p_in->z;

    short cosY = fastCos(angleY);
    short sinY = fastSin(angleY);
    temp_x = (mulViaLogExp(x, cosY) + mulViaLogExp(z, sinY));
    temp_z = (mulViaLogExp(x, sinY) + mulViaLogExp(z, cosY));
    x = temp_x;
    z = temp_z;

    short cosX = fastCos(angleX);
    short sinX = fastSin(angleX);
    temp_y = (mulViaLogExp(y, cosX) + mulViaLogExp(z, sinX));
    temp_z = (mulViaLogExp(y, sinX) + mulViaLogExp(z, cosX));
    y = temp_y;
    z = temp_z;

//    printf("in: %f %f %f\n", fixed_fp(x), fixed_fp(y), fixed_fp(z));

    p_out.x = (short)x;
    p_out.y = (short)y;
    p_out.z = (short)z;

    return p_out;
}


// Project integer 3D point to 2D screen coordinates
static inline Point2D project(Point3DInt p) {
#define SCREEN_WIDTH_HALF (SCREEN_WIDTH / 2)
#define SCREEN_HEIGHT_HALF (SCREEN_HEIGHT / 2)
    Point2D projected;

    projected.x = SCREEN_WIDTH_HALF + (p.x >> (FP_SHIFT-5));
    projected.y = SCREEN_HEIGHT_HALF + (p.y >> (FP_SHIFT-5)) - (p.z >> (FP_SHIFT-4));

    return projected;
}

void render(short angleY, short angleX) {
    Point2D projectedVertices[NUM_VERTICES];
    unsigned short i;

    for (i = 0; i < NUM_VERTICES; ++i) {
        Point3DInt transform = rotate(i, angleY, angleX);
        projectedVertices[i] = project(transform);
    }

    Line lines[NUM_EDGES+1];
    memset(&lines[NUM_EDGES], 0, sizeof(Line));
    for (i = 0; i < NUM_EDGES; ++i) {
        int v1_idx = sv2025Edges[i][0];
        int v2_idx = sv2025Edges[i][1];

	    Point2D p1 = projectedVertices[v1_idx];
	    Point2D p2 = projectedVertices[v2_idx];


	//SegmentedLine(p1.x, p1.y, p2.x, p2.y, gDrawingBuffer);
	//draw_line(p1.x, p1.y, p2.x, p2.y, COLOR_FOREGROUND, gDrawingBuffer);

        // TODO smart clipping
        #define min_y 1
        #define max_y 199
    	if (p1.y < min_y) {
    	     p1.y = 1;
    	}
    	if (p2.y < min_y) {
    	    p2.y = 1;
    	}
    	if (p1.y > max_y) {
    	    p1.y = 199;
    	}
    	if (p2.y > max_y) {
    	    p2.y = 199;
    	}

        #define min_x 1
        #define max_x 319
    	if (p1.x < min_x) {
    	     p1.x = 1;
    	}
    	if (p2.x < min_x) {
    	    p2.x = 1;
    	}
    	if (p1.x > max_x) {
    	    p1.x = max_x;
    	}
    	if (p2.x > max_x) {
    	    p2.x = max_x;
    	}

        lines[i].p0 = p1;
	    lines[i].p1 = p2;
    }

    SegmentedMultiLine(lines, gDrawingBuffer);
}

int check_input() {
    if (Bconstat(2) != 0) {
        long key = Bconin(2);
        unsigned char ascii = key & 0xFF;
        if (ascii == ' ' || ascii == 27 ) {
            return 0;
        }
    }
    return 0;
}

int
main(int argc, char * argv[])
{
    (void)argc;
    (void)argv;
    initLUTs();

    if (!init_system()) {
        (void)Cconws("System initialization failed!\r\n");
        return 1;
    }

    SegmentedLineSetup();

    model_scale();

    short angleY = 0;
    short angleX = 0;
    short angleYinc = (short)(0.08 * FP_ONE);
    short angleXinc = (short)(0.13 * FP_ONE);
    angleYinc = (angleYinc * LUT_SIZE) / (2 * FP_ONE * 31415 / 10000);
    angleXinc = (angleXinc * LUT_SIZE) / (2 * FP_ONE * 31415 / 10000);

    while (!check_input()) {
    	memset(gDrawingBuffer, 0, SCREEN_SIZE_BYTES);

        angleY += angleYinc;
	    angleX += angleXinc;

	    render(angleY, angleX);
    	Setscreen(gDrawingBuffer, gDrawingBuffer, -1);
	    Vsync();

        void* temp = gActiveBuffer;
        gActiveBuffer = gDrawingBuffer;
        gDrawingBuffer = temp;
    }

    (void)Cconws("End!\r\n");

    restore_system();
    return 0;
}
