#ifndef _SANDEC_H_
#define _SANDEC_H_

#include <inttypes.h>
#include <sys/types.h>

#define NGLYPHS 256

struct sanio {
	void *ctx;
	int(*read)(void *ctx, void *dst, uint32_t size);
};

struct sanrt {
	struct sanio *io;
	uint32_t _bsz;		// current block size
	uint32_t totalsize;
	uint32_t currframe;
	uint32_t framerate;
	uint32_t maxframe;
	uint16_t FRMEcnt;
	uint16_t w;  // frame width/pitch/stride
	uint16_t h;  // frame height
	uint16_t version;

	int(*queue_video)(struct sanrt *san, unsigned char *vdata, uint32_t size, int newpal);
	int(*queue_audio)(struct sanrt *san, unsigned char *adata, uint32_t size);

	// codec47 stuff
	int8_t glyph4x4[NGLYPHS][16];
	int8_t glyph8x8[NGLYPHS][64];
	unsigned long fbsize;	// size of the buffers below
	unsigned char *buf0;
	unsigned char *buf1;
	unsigned char *buf2;
	unsigned char *buf3;	// aux buffer for "STOR" and "FTCH"
	unsigned char *buf;	// baseptr
	uint32_t lastseq;
	uint32_t rotate;
	int to_store;

	// iact block stuff
	uint32_t samplerate;
	uint16_t iactpos;

	// buffers
	uint32_t palette[256]; // ABGR
	uint16_t deltapal[768];
	uint8_t iactbuf[4096];
	void *userdata;
	int newpal;
};

struct sanctx {
	struct sanrt *rt;
	struct sanio *io;
	void *userdata;
};

int san_init(struct sanrt** sanctx);

int san_open(struct sanrt* rt, struct sanio *io);

int san_one_frame(struct sanrt *rt);

#endif
