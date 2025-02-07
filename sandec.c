/*
 * A/V decoder for LucasArts Outlaws ".SAN" and ".NUT" video files.
 *  SMUSH Video codecs 1/47/48 with 8-bit palletized 640x480 video,
 *  IACT scaled audio in 22,05kHz 16bit Stereo Little-endian format.
 *
 * Written in 2024-2025 by Manuel Lauss <manuel.lauss@gmail.com>
 *
 * Codec algorithms (Video, Audio, Palette) liberally taken from FFmpeg,
 * ScummVM, and by looking at the various game EXEs with Ghidra.
 * https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/HEAD:/libavcodec/sanm.c
 * https://github.com/scummvm/scummvm/blob/master/engines/scumm/smush/smush_player.cpp
 * https://github.com/clone2727/smushplay/blob/master/codec47.cpp
 * https://github.com/clone2727/smushplay/blob/master/codec48.cpp
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <memory.h>
#include <stdlib.h>
#include "sandec.h"

#ifndef _max
#define _max(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef _min
#define _min(a,b) ((a) > (b) ? (b) : (a))
#endif

#define bswap_16(value) \
	((((value) & 0xff) << 8) | ((value) >> 8))

#define bswap_32(value) \
	(((uint32_t)bswap_16((uint16_t)((value) & 0xffff)) << 16) | \
	  (uint32_t)bswap_16((uint16_t)((value) >> 16)))

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)
#define be32_to_cpu(x) bswap_32(x)
#define be16_to_cpu(x) bswap_16(x)
#define cpu_to_le16(x) (x)

/* read an unaligned 16bit value from memory */
static inline uint16_t ua16(uint8_t *p)
{
	return p[0] | p[1] << 8;
}

/* read an unaligned 32bit value from memory */
static inline uint32_t ua32(uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

#define be32_to_cpu(x)  (x)
#define be16_to_cpu(x)  (x)
#define le32_to_cpu(x)  bswap_32(x)
#define le16_to_cpu(x)  bswap_16(x)
#define cpu_to_le16(x)  bswap_16(x)

/* read an unaligned 16bit value from memory */
static inline uint16_t ua16(uint8_t *p)
{
	return p[1] | p[0] << 8;
}

/* read an unaligned 32bit value from memory */
static inline uint32_t ua32(uint8_t *p)
{
	return p[3] | p[2] << 8 | p[1] << 16 | p[0] << 24;
}

#else

#error "unknown endianness"

#endif

/* sizes of various internal static buffers */
#define SZ_IACT		(4096)
#define SZ_PAL		(256 * 4)
#define SZ_DELTAPAL	(768 * 2)
#define SZ_C47IPTBL	(256 * 256)
#define SZ_AUDIOOUT	(4096)
#define SZ_AUDTMPBUF1	(262144)
#define SZ_ALL (SZ_IACT + SZ_PAL + SZ_DELTAPAL + SZ_C47IPTBL + SZ_AUDIOOUT + \
		SZ_AUDTMPBUF1)


/* chunk identifiers LE */
#define ANIM	0x4d494e41
#define AHDR	0x52444841
#define FRME	0x454d5246
#define NPAL	0x4c41504e
#define FOBJ	0x4a424f46
#define IACT	0x54434149
#define TRES	0x53455254
#define STOR	0x524f5453
#define FTCH	0x48435446
#define XPAL	0x4c415058
#define iMUS	0x53554d69
#define MAP_	0x2050414d
#define FRMT	0x544d5246
#define DATA	0x41544144
#define PSAD	0x44415350
#define SAUD	0x44554153
#define STRK	0x4B525453
#define SDAT	0x54414453


/* codec47 glyhps */
#define GLYPH_COORD_VECT_SIZE 16
#define NGLYPHS 256

#define AUD_INUSE		(1 << 0)
#define AUD_SRC8BIT		(1 << 1)
#define AUD_SRC12BIT		(1 << 2)
#define AUD_1CH			(1 << 3)
#define AUD_RATE11KHZ		(1 << 4)
#define AUD_SRCDONE		(1 << 5)
#define AUD_MIXED		(1 << 6)

/* audio track. 256k buffer is enough for The Dig sq1.san */
#define ATRK_MAXWP	((1 << 18) - 26)
/* 8 seems enough for The Dig */
#define ATRK_NUM	8
struct sanatrk {
	uint8_t data[ATRK_MAXWP];/* 22,05kHz signed 16bit 2ch audio data	*/
	uint32_t rdptr;
	uint32_t wrptr;
	uint32_t datacnt;	/* currently held data in buffer	*/
	uint32_t flags;		/* track flags				*/
	uint32_t pdcnt;		/* count of left over bytes from prev.  */
	uint8_t pd[4];		/* unused bytes from prev. datablock	*/
	uint16_t trkid;		/* ID of this track			*/
	int32_t dataleft;	/* SAUD data left until track ends	*/
};

/* internal context: per-file */
struct sanrt {
	uint32_t frmebufsz;	/* 4 size of buffer below		*/
	uint8_t *fcache;	/* 8 one cached FRME object		*/
	uint8_t *buf0;		/* 8 current front buffer		*/
	uint8_t *buf1;		/* 8 c47 delta buffer 1			*/
	uint8_t *buf2;		/* 8 c47 delta buffer 2			*/
	uint8_t *buf3;		/* 8 STOR buffer			*/
	uint8_t *buf4;		/* 8 last full frame for interpolation  */
	uint8_t *buf5;		/* 8 interpolated frame                 */
	uint8_t *vbuf;		/* 8 final image buffer passed to caller*/
	uint8_t *abuf;		/* 8 audio output buffer		*/
	uint16_t pitch;		/* 2 image pitch			*/
	uint16_t bufw;		/* 2 alloc'ed buffer width/pitch	*/
	uint16_t bufh;		/* 2 alloc'ed buffer height		*/
	uint16_t frmw;		/* 2 current frame width		*/
	uint16_t frmh;		/* 2 current frame height		*/
	int16_t  lastseq;	/* 2 c47 last sequence id		*/
	uint16_t subid;		/* 2 subtitle message number		*/
	uint16_t to_store;	/* 2 STOR encountered			*/
	uint16_t currframe;	/* 2 current frame index		*/
	uint16_t iactpos;	/* 2 IACT buffer write pointer		*/
	uint8_t *iactbuf;	/* 8 4kB for IACT chunks 		*/
	uint8_t *c47ipoltbl;	/* 8 c47 interpolation table Compression 1 */
	int16_t  *deltapal;	/* 8 768x 16bit for XPAL chunks		*/
	uint32_t *palette;	/* 8 256x ABGR				*/
	uint8_t  *buf;		/* 8 fb baseptr				*/
	uint32_t fbsize;	/* 4 size of the framebuffers		*/
	uint32_t framedur;	/* 4 standard frame duration		*/
	uint32_t samplerate;	/* 4 audio samplerate in Hz		*/
	uint16_t FRMEcnt;	/* 2 number of FRMEs in SAN		*/
	uint16_t version;	/* 2 SAN version number			*/
	uint8_t  have_vdims:1;	/* 1 we have valid video dimensions	*/
	uint8_t  have_frame:1;	/* 1 we have a valid video frame	*/
	uint8_t  have_itable:1;	/* 1 have c47/48 interpolation table    */
	uint8_t  can_ipol:1;	/* 1 do an interpolation                */
	uint8_t  have_ipframe:1;/* 1 we have an interpolated frame      */
	struct sanatrk sanatrk[ATRK_NUM];	/* audio tracks 	*/
	uint8_t  acttrks;	/* 1 active audio tracks in this frame	*/
	uint8_t *atmpbuf1;	/* 8 audio buffer 1			*/
	uint8_t psadhdr;	/* 1 psad type 1 = old 2 new		*/
};

/* internal context: static stuff. */
struct sanctx {
	struct sanrt rt;
	struct sanio *io;
	int errdone;		/* latest error status */

	/* codec47 static data */
	int8_t c47_glyph4x4[NGLYPHS][16];
	int8_t c47_glyph8x8[NGLYPHS][64];
};

/* Codec37/Codec48 motion vectors */
static const int8_t c37_mv[3][510] = {
	{
	0,   0,   1,   0,   2,   0,   3,   0,   5,   0,   8,   0,  13,   0,  21,
	0,  -1,   0,  -2,   0,  -3,   0,  -5,   0,  -8,   0, -13,   0, -17,   0,
	-21,   0,   0,   1,   1,   1,   2,   1,   3,   1,   5,   1,   8,   1,  13,
	1,  21,   1,  -1,   1,  -2,   1,  -3,   1,  -5,   1,  -8,   1, -13,   1,
	-17,   1, -21,   1,   0,   2,   1,   2,   2,   2,   3,   2,   5,   2,   8,
	2,  13,   2,  21,   2,  -1,   2,  -2,   2,  -3,   2,  -5,   2,  -8,   2,
	-13,   2, -17,   2, -21,   2,   0,   3,   1,   3,   2,   3,   3,   3,   5,
	3,   8,   3,  13,   3,  21,   3,  -1,   3,  -2,   3,  -3,   3,  -5,   3,
	-8,   3, -13,   3, -17,   3, -21,   3,   0,   5,   1,   5,   2,   5,   3,
	5,   5,   5,   8,   5,  13,   5,  21,   5,  -1,   5,  -2,   5,  -3,   5,
	-5,   5,  -8,   5, -13,   5, -17,   5, -21,   5,   0,   8,   1,   8,   2,
	8,   3,   8,   5,   8,   8,   8,  13,   8,  21,   8,  -1,   8,  -2,   8,
	-3,   8,  -5,   8,  -8,   8, -13,   8, -17,   8, -21,   8,   0,  13,   1,
	13,   2,  13,   3,  13,   5,  13,   8,  13,  13,  13,  21,  13,  -1,  13,
	-2,  13,  -3,  13,  -5,  13,  -8,  13, -13,  13, -17,  13, -21,  13,   0,
	21,   1,  21,   2,  21,   3,  21,   5,  21,   8,  21,  13,  21,  21,  21,
	-1,  21,  -2,  21,  -3,  21,  -5,  21,  -8,  21, -13,  21, -17,  21, -21,
	21,   0,  -1,   1,  -1,   2,  -1,   3,  -1,   5,  -1,   8,  -1,  13,  -1,
	21,  -1,  -1,  -1,  -2,  -1,  -3,  -1,  -5,  -1,  -8,  -1, -13,  -1, -17,
	-1, -21,  -1,   0,  -2,   1,  -2,   2,  -2,   3,  -2,   5,  -2,   8,  -2,
	13,  -2,  21,  -2,  -1,  -2,  -2,  -2,  -3,  -2,  -5,  -2,  -8,  -2, -13,
	-2, -17,  -2, -21,  -2,   0,  -3,   1,  -3,   2,  -3,   3,  -3,   5,  -3,
	8,  -3,  13,  -3,  21,  -3,  -1,  -3,  -2,  -3,  -3,  -3,  -5,  -3,  -8,
	-3, -13,  -3, -17,  -3, -21,  -3,   0,  -5,   1,  -5,   2,  -5,   3,  -5,
	5,  -5,   8,  -5,  13,  -5,  21,  -5,  -1,  -5,  -2,  -5,  -3,  -5,  -5,
	-5,  -8,  -5, -13,  -5, -17,  -5, -21,  -5,   0,  -8,   1,  -8,   2,  -8,
	3,  -8,   5,  -8,   8,  -8,  13,  -8,  21,  -8,  -1,  -8,  -2,  -8,  -3,
	-8,  -5,  -8,  -8,  -8, -13,  -8, -17,  -8, -21,  -8,   0, -13,   1, -13,
	2, -13,   3, -13,   5, -13,   8, -13,  13, -13,  21, -13,  -1, -13,  -2,
	-13,  -3, -13,  -5, -13,  -8, -13, -13, -13, -17, -13, -21, -13,   0, -17,
	1, -17,   2, -17,   3, -17,   5, -17,   8, -17,  13, -17,  21, -17,  -1,
	-17,  -2, -17,  -3, -17,  -5, -17,  -8, -17, -13, -17, -17, -17, -21, -17,
	0, -21,   1, -21,   2, -21,   3, -21,   5, -21,   8, -21,  13, -21,  21,
	-21,  -1, -21,  -2, -21,  -3, -21,  -5, -21,  -8, -21, -13, -21, -17, -21
	},
	 {
	0,   0,  -8, -29,   8, -29, -18, -25,  17, -25,   0, -23,  -6, -22,   6,
	-22, -13, -19,  12, -19,   0, -18,  25, -18, -25, -17,  -5, -17,   5, -17,
	-10, -15,  10, -15,   0, -14,  -4, -13,   4, -13,  19, -13, -19, -12,  -8,
	-11,  -2, -11,   0, -11,   2, -11,   8, -11, -15, -10,  -4, -10,   4, -10,
	15, -10,  -6,  -9,  -1,  -9,   1,  -9,   6,  -9, -29,  -8, -11,  -8,  -8,
	-8,  -3,  -8,   3,  -8,   8,  -8,  11,  -8,  29,  -8,  -5,  -7,  -2,  -7,
	0,  -7,   2,  -7,   5,  -7, -22,  -6,  -9,  -6,  -6,  -6,  -3,  -6,  -1,
	-6,   1,  -6,   3,  -6,   6,  -6,   9,  -6,  22,  -6, -17,  -5,  -7,  -5,
	-4,  -5,  -2,  -5,   0,  -5,   2,  -5,   4,  -5,   7,  -5,  17,  -5, -13,
	-4, -10,  -4,  -5,  -4,  -3,  -4,  -1,  -4,   0,  -4,   1,  -4,   3,  -4,
	5,  -4,  10,  -4,  13,  -4,  -8,  -3,  -6,  -3,  -4,  -3,  -3,  -3,  -2,
	-3,  -1,  -3,   0,  -3,   1,  -3,   2,  -3,   4,  -3,   6,  -3,   8,  -3,
	-11,  -2,  -7,  -2,  -5,  -2,  -3,  -2,  -2,  -2,  -1,  -2,   0,  -2,   1,
	-2,   2,  -2,   3,  -2,   5,  -2,   7,  -2,  11,  -2,  -9,  -1,  -6,  -1,
	-4,  -1,  -3,  -1,  -2,  -1,  -1,  -1,   0,  -1,   1,  -1,   2,  -1,   3,
	-1,   4,  -1,   6,  -1,   9,  -1, -31,   0, -23,   0, -18,   0, -14,   0,
	-11,   0,  -7,   0,  -5,   0,  -4,   0,  -3,   0,  -2,   0,  -1,   0,   0,
	-31,   1,   0,   2,   0,   3,   0,   4,   0,   5,   0,   7,   0,  11,   0,
	14,   0,  18,   0,  23,   0,  31,   0,  -9,   1,  -6,   1,  -4,   1,  -3,
	1,  -2,   1,  -1,   1,   0,   1,   1,   1,   2,   1,   3,   1,   4,   1,
	6,   1,   9,   1, -11,   2,  -7,   2,  -5,   2,  -3,   2,  -2,   2,  -1,
	2,   0,   2,   1,   2,   2,   2,   3,   2,   5,   2,   7,   2,  11,   2,
	-8,   3,  -6,   3,  -4,   3,  -2,   3,  -1,   3,   0,   3,   1,   3,   2,
	3,   3,   3,   4,   3,   6,   3,   8,   3, -13,   4, -10,   4,  -5,   4,
	-3,   4,  -1,   4,   0,   4,   1,   4,   3,   4,   5,   4,  10,   4,  13,
	4, -17,   5,  -7,   5,  -4,   5,  -2,   5,   0,   5,   2,   5,   4,   5,
	7,   5,  17,   5, -22,   6,  -9,   6,  -6,   6,  -3,   6,  -1,   6,   1,
	6,   3,   6,   6,   6,   9,   6,  22,   6,  -5,   7,  -2,   7,   0,   7,
	2,   7,   5,   7, -29,   8, -11,   8,  -8,   8,  -3,   8,   3,   8,   8,
	8,  11,   8,  29,   8,  -6,   9,  -1,   9,   1,   9,   6,   9, -15,  10,
	-4,  10,   4,  10,  15,  10,  -8,  11,  -2,  11,   0,  11,   2,  11,   8,
	11,  19,  12, -19,  13,  -4,  13,   4,  13,   0,  14, -10,  15,  10,  15,
	-5,  17,   5,  17,  25,  17, -25,  18,   0,  18, -12,  19,  13,  19,  -6,
	22,   6,  22,   0,  23, -17,  25,  18,  25,  -8,  29,   8,  29,   0,  31
	}, {
	0,   0,  -6, -22,   6, -22, -13, -19,  12, -19,   0, -18,  -5, -17,   5,
	-17, -10, -15,  10, -15,   0, -14,  -4, -13,   4, -13,  19, -13, -19, -12,
	-8, -11,  -2, -11,   0, -11,   2, -11,   8, -11, -15, -10,  -4, -10,   4,
	-10,  15, -10,  -6,  -9,  -1,  -9,   1,  -9,   6,  -9, -11,  -8,  -8,  -8,
	-3,  -8,   0,  -8,   3,  -8,   8,  -8,  11,  -8,  -5,  -7,  -2,  -7,   0,
	-7,   2,  -7,   5,  -7, -22,  -6,  -9,  -6,  -6,  -6,  -3,  -6,  -1,  -6,
	1,  -6,   3,  -6,   6,  -6,   9,  -6,  22,  -6, -17,  -5,  -7,  -5,  -4,
	-5,  -2,  -5,  -1,  -5,   0,  -5,   1,  -5,   2,  -5,   4,  -5,   7,  -5,
	17,  -5, -13,  -4, -10,  -4,  -5,  -4,  -3,  -4,  -2,  -4,  -1,  -4,   0,
	-4,   1,  -4,   2,  -4,   3,  -4,   5,  -4,  10,  -4,  13,  -4,  -8,  -3,
	-6,  -3,  -4,  -3,  -3,  -3,  -2,  -3,  -1,  -3,   0,  -3,   1,  -3,   2,
	-3,   3,  -3,   4,  -3,   6,  -3,   8,  -3, -11,  -2,  -7,  -2,  -5,  -2,
	-4,  -2,  -3,  -2,  -2,  -2,  -1,  -2,   0,  -2,   1,  -2,   2,  -2,   3,
	-2,   4,  -2,   5,  -2,   7,  -2,  11,  -2,  -9,  -1,  -6,  -1,  -5,  -1,
	-4,  -1,  -3,  -1,  -2,  -1,  -1,  -1,   0,  -1,   1,  -1,   2,  -1,   3,
	-1,   4,  -1,   5,  -1,   6,  -1,   9,  -1, -23,   0, -18,   0, -14,   0,
	-11,   0,  -7,   0,  -5,   0,  -4,   0,  -3,   0,  -2,   0,  -1,   0,   0,
	-23,   1,   0,   2,   0,   3,   0,   4,   0,   5,   0,   7,   0,  11,   0,
	14,   0,  18,   0,  23,   0,  -9,   1,  -6,   1,  -5,   1,  -4,   1,  -3,
	1,  -2,   1,  -1,   1,   0,   1,   1,   1,   2,   1,   3,   1,   4,   1,
	5,   1,   6,   1,   9,   1, -11,   2,  -7,   2,  -5,   2,  -4,   2,  -3,
	2,  -2,   2,  -1,   2,   0,   2,   1,   2,   2,   2,   3,   2,   4,   2,
	5,   2,   7,   2,  11,   2,  -8,   3,  -6,   3,  -4,   3,  -3,   3,  -2,
	3,  -1,   3,   0,   3,   1,   3,   2,   3,   3,   3,   4,   3,   6,   3,
	8,   3, -13,   4, -10,   4,  -5,   4,  -3,   4,  -2,   4,  -1,   4,   0,
	4,   1,   4,   2,   4,   3,   4,   5,   4,  10,   4,  13,   4, -17,   5,
	-7,   5,  -4,   5,  -2,   5,  -1,   5,   0,   5,   1,   5,   2,   5,   4,
	5,   7,   5,  17,   5, -22,   6,  -9,   6,  -6,   6,  -3,   6,  -1,   6,
	1,   6,   3,   6,   6,   6,   9,   6,  22,   6,  -5,   7,  -2,   7,   0,
	7,   2,   7,   5,   7, -11,   8,  -8,   8,  -3,   8,   0,   8,   3,   8,
	8,   8,  11,   8,  -6,   9,  -1,   9,   1,   9,   6,   9, -15,  10,  -4,
	10,   4,  10,  15,  10,  -8,  11,  -2,  11,   0,  11,   2,  11,   8,  11,
	19,  12, -19,  13,  -4,  13,   4,  13,   0,  14, -10,  15,  10,  15,  -5,
	17,   5,  17,   0,  18, -12,  19,  13,  19,  -6,  22,   6,  22,   0,  23
	}
};


/******************************************************************************
 * SAN Codec47 Glyph setup, taken from ffmpeg
 * https://git.ffmpeg.org/gitweb/ffmpeg.git/blob_plain/HEAD:/libavcodec/sanm.c
 */

static const int8_t c47_glyph4_x[GLYPH_COORD_VECT_SIZE] = {
	0, 1, 2, 3, 3, 3, 3, 2, 1, 0, 0, 0, 1, 2, 2, 1
};

static const int8_t c47_glyph4_y[GLYPH_COORD_VECT_SIZE] = {
	0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 2, 1, 1, 1, 2, 2
};

static const int8_t c47_glyph8_x[GLYPH_COORD_VECT_SIZE] = {
	0, 2, 5, 7, 7, 7, 7, 7, 7, 5, 2, 0, 0, 0, 0, 0
};

static const int8_t c47_glyph8_y[GLYPH_COORD_VECT_SIZE] = {
	0, 0, 0, 0, 1, 3, 4, 6, 7, 7, 7, 7, 6, 4, 3, 1
};

static const int8_t c47_mv[256][2] = {
	{   0,   0 }, {  -1, -43 }, {   6, -43 }, {  -9, -42 }, {  13, -41 },
	{ -16, -40 }, {  19, -39 }, { -23, -36 }, {  26, -34 }, {  -2, -33 },
	{   4, -33 }, { -29, -32 }, {  -9, -32 }, {  11, -31 }, { -16, -29 },
	{  32, -29 }, {  18, -28 }, { -34, -26 }, { -22, -25 }, {  -1, -25 },
	{   3, -25 }, {  -7, -24 }, {   8, -24 }, {  24, -23 }, {  36, -23 },
	{ -12, -22 }, {  13, -21 }, { -38, -20 }, {   0, -20 }, { -27, -19 },
	{  -4, -19 }, {   4, -19 }, { -17, -18 }, {  -8, -17 }, {   8, -17 },
	{  18, -17 }, {  28, -17 }, {  39, -17 }, { -12, -15 }, {  12, -15 },
	{ -21, -14 }, {  -1, -14 }, {   1, -14 }, { -41, -13 }, {  -5, -13 },
	{   5, -13 }, {  21, -13 }, { -31, -12 }, { -15, -11 }, {  -8, -11 },
	{   8, -11 }, {  15, -11 }, {  -2, -10 }, {   1, -10 }, {  31, -10 },
	{ -23,  -9 }, { -11,  -9 }, {  -5,  -9 }, {   4,  -9 }, {  11,  -9 },
	{  42,  -9 }, {   6,  -8 }, {  24,  -8 }, { -18,  -7 }, {  -7,  -7 },
	{  -3,  -7 }, {  -1,  -7 }, {   2,  -7 }, {  18,  -7 }, { -43,  -6 },
	{ -13,  -6 }, {  -4,  -6 }, {   4,  -6 }, {   8,  -6 }, { -33,  -5 },
	{  -9,  -5 }, {  -2,  -5 }, {   0,  -5 }, {   2,  -5 }, {   5,  -5 },
	{  13,  -5 }, { -25,  -4 }, {  -6,  -4 }, {  -3,  -4 }, {   3,  -4 },
	{   9,  -4 }, { -19,  -3 }, {  -7,  -3 }, {  -4,  -3 }, {  -2,  -3 },
	{  -1,  -3 }, {   0,  -3 }, {   1,  -3 }, {   2,  -3 }, {   4,  -3 },
	{   6,  -3 }, {  33,  -3 }, { -14,  -2 }, { -10,  -2 }, {  -5,  -2 },
	{  -3,  -2 }, {  -2,  -2 }, {  -1,  -2 }, {   0,  -2 }, {   1,  -2 },
	{   2,  -2 }, {   3,  -2 }, {   5,  -2 }, {   7,  -2 }, {  14,  -2 },
	{  19,  -2 }, {  25,  -2 }, {  43,  -2 }, {  -7,  -1 }, {  -3,  -1 },
	{  -2,  -1 }, {  -1,  -1 }, {   0,  -1 }, {   1,  -1 }, {   2,  -1 },
	{   3,  -1 }, {  10,  -1 }, {  -5,   0 }, {  -3,   0 }, {  -2,   0 },
	{  -1,   0 }, {   1,   0 }, {   2,   0 }, {   3,   0 }, {   5,   0 },
	{   7,   0 }, { -10,   1 }, {  -7,   1 }, {  -3,   1 }, {  -2,   1 },
	{  -1,   1 }, {   0,   1 }, {   1,   1 }, {   2,   1 }, {   3,   1 },
	{ -43,   2 }, { -25,   2 }, { -19,   2 }, { -14,   2 }, {  -5,   2 },
	{  -3,   2 }, {  -2,   2 }, {  -1,   2 }, {   0,   2 }, {   1,   2 },
	{   2,   2 }, {   3,   2 }, {   5,   2 }, {   7,   2 }, {  10,   2 },
	{  14,   2 }, { -33,   3 }, {  -6,   3 }, {  -4,   3 }, {  -2,   3 },
	{  -1,   3 }, {   0,   3 }, {   1,   3 }, {   2,   3 }, {   4,   3 },
	{  19,   3 }, {  -9,   4 }, {  -3,   4 }, {   3,   4 }, {   7,   4 },
	{  25,   4 }, { -13,   5 }, {  -5,   5 }, {  -2,   5 }, {   0,   5 },
	{   2,   5 }, {   5,   5 }, {   9,   5 }, {  33,   5 }, {  -8,   6 },
	{  -4,   6 }, {   4,   6 }, {  13,   6 }, {  43,   6 }, { -18,   7 },
	{  -2,   7 }, {   0,   7 }, {   2,   7 }, {   7,   7 }, {  18,   7 },
	{ -24,   8 }, {  -6,   8 }, { -42,   9 }, { -11,   9 }, {  -4,   9 },
	{   5,   9 }, {  11,   9 }, {  23,   9 }, { -31,  10 }, {  -1,  10 },
	{   2,  10 }, { -15,  11 }, {  -8,  11 }, {   8,  11 }, {  15,  11 },
	{  31,  12 }, { -21,  13 }, {  -5,  13 }, {   5,  13 }, {  41,  13 },
	{  -1,  14 }, {   1,  14 }, {  21,  14 }, { -12,  15 }, {  12,  15 },
	{ -39,  17 }, { -28,  17 }, { -18,  17 }, {  -8,  17 }, {   8,  17 },
	{  17,  18 }, {  -4,  19 }, {   0,  19 }, {   4,  19 }, {  27,  19 },
	{  38,  20 }, { -13,  21 }, {  12,  22 }, { -36,  23 }, { -24,  23 },
	{  -8,  24 }, {   7,  24 }, {  -3,  25 }, {   1,  25 }, {  22,  25 },
	{  34,  26 }, { -18,  28 }, { -32,  29 }, {  16,  29 }, { -11,  31 },
	{   9,  32 }, {  29,  32 }, {  -4,  33 }, {   2,  33 }, { -26,  34 },
	{  23,  36 }, { -19,  39 }, {  16,  40 }, { -13,  41 }, {   9,  42 },
	{  -6,  43 }, {   1,  43 }, {   0,   0 }, {   0,   0 }, {   0,   0 },
};

enum GlyphEdge {
	LEFT_EDGE,
	TOP_EDGE,
	RIGHT_EDGE,
	BOTTOM_EDGE,
	NO_EDGE
};

enum GlyphDir {
	DIR_LEFT,
	DIR_UP,
	DIR_RIGHT,
	DIR_DOWN,
	NO_DIR
};

static enum GlyphEdge c47_which_edge(int x, int y, int edge_size)
{
	const int edge_max = edge_size - 1;

	if (!y)
		return BOTTOM_EDGE;
	else if (y == edge_max)
		return TOP_EDGE;
	else if (!x)
		return LEFT_EDGE;
	else if (x == edge_max)
		return RIGHT_EDGE;
	else
		return NO_EDGE;
}

static enum GlyphDir c47_which_direction(enum GlyphEdge edge0, enum GlyphEdge edge1)
{
	if ((edge0 == LEFT_EDGE && edge1 == RIGHT_EDGE) ||
		(edge1 == LEFT_EDGE && edge0 == RIGHT_EDGE) ||
		(edge0 == BOTTOM_EDGE && edge1 != TOP_EDGE) ||
		(edge1 == BOTTOM_EDGE && edge0 != TOP_EDGE))
		return DIR_UP;
	else if ((edge0 == TOP_EDGE && edge1 != BOTTOM_EDGE) ||
		(edge1 == TOP_EDGE && edge0 != BOTTOM_EDGE))
		return DIR_DOWN;
	else if ((edge0 == LEFT_EDGE && edge1 != RIGHT_EDGE) ||
		(edge1 == LEFT_EDGE && edge0 != RIGHT_EDGE))
		return DIR_LEFT;
	else if ((edge0 == TOP_EDGE && edge1 == BOTTOM_EDGE) ||
		(edge1 == TOP_EDGE && edge0 == BOTTOM_EDGE) ||
		(edge0 == RIGHT_EDGE && edge1 != LEFT_EDGE) ||
		(edge1 == RIGHT_EDGE && edge0 != LEFT_EDGE))
		return DIR_RIGHT;

	return NO_DIR;
}

/* Interpolate two points. */
static void c47_interp_point(int8_t *points, int x0, int y0, int x1, int y1,
			 int pos, int npoints)
{
	if (npoints) {
		points[0] = (x0 * pos + x1 * (npoints - pos) + (npoints >> 1)) / npoints;
		points[1] = (y0 * pos + y1 * (npoints - pos) + (npoints >> 1)) / npoints;
	} else {
		points[0] = x0;
		points[1] = y0;
	}
}

static void c47_make_glyphs(int8_t *pglyphs, const int8_t *xvec, const int8_t *yvec,
			const int side_length)
{
	const int glyph_size = side_length * side_length;
	int8_t *pglyph = pglyphs;

	int i, j;
	for (i = 0; i < GLYPH_COORD_VECT_SIZE; i++) {
		int x0 = xvec[i];
		int y0 = yvec[i];
		enum GlyphEdge edge0 = c47_which_edge(x0, y0, side_length);

		for (j = 0; j < GLYPH_COORD_VECT_SIZE; j++, pglyph += glyph_size) {
			int x1 = xvec[j];
			int y1 = yvec[j];
			enum GlyphEdge edge1 = c47_which_edge(x1, y1, side_length);
			enum GlyphDir dir = c47_which_direction(edge0, edge1);
			int npoints = _max(abs(x1 - x0), abs(y1 - y0));
			int ipoint;

			for (ipoint = 0; ipoint <= npoints; ipoint++) {
				int8_t point[2];
				int irow, icol;

				c47_interp_point(point, x0, y0, x1, y1, ipoint, npoints);

				switch (dir) {
					case DIR_UP:
						for (irow = point[1]; irow >= 0; irow--)
							pglyph[point[0] + irow * side_length] = 1;
					break;

					case DIR_DOWN:
						for (irow = point[1]; irow < side_length; irow++)
							pglyph[point[0] + irow * side_length] = 1;
					break;

					case DIR_LEFT:
						for (icol = point[0]; icol >= 0; icol--)
							pglyph[icol + point[1] * side_length] = 1;
					break;

					case DIR_RIGHT:
						for (icol = point[0]; icol < side_length; icol++)
							pglyph[icol + point[1] * side_length] = 1;
					break;

					case NO_DIR:
					break;
				}
			}
		}
	}
}


/******************************************************************************/

/* allocate memory for a full FRME */
static int allocfrme(struct sanctx *ctx, uint32_t sz)
{
	sz = (sz + 31) & ~31;
	if (sz > ctx->rt.frmebufsz) {
		if (ctx->rt.fcache)
			free(ctx->rt.fcache);
		ctx->rt.fcache = (uint8_t *)malloc(sz);
		if (!ctx->rt.fcache) {
			ctx->rt.frmebufsz = 0;
			return 1;
		}
		ctx->rt.frmebufsz = sz;
	}
	return 0;
}

static inline int read_source(struct sanctx *ctx, void *dst, uint32_t sz)
{
	return !(ctx->io->ioread(ctx->io->userctx, dst, sz));
}

static void read_palette(struct sanctx *ctx, uint8_t *src)
{
	struct sanrt *rt = &ctx->rt;
	uint32_t *pal = rt->palette;
	uint8_t t[12];
	int i = 0;

	while (i < 256) {
		t[0] = *src++;
		t[1] = *src++;
		t[2] = *src++;
		*pal++ = 0xff << 24 | t[2] << 16 | t[1] << 8 | t[0];
		i++;
	}
}

static void interpolate_frame(uint8_t *dst, const uint8_t *sr1, const uint8_t *srs,
			      const uint8_t *itbl, const uint16_t w, const uint16_t h)
{
	int i, j, k;

	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++) {
			k = (*sr1++) << 8 | (*srs++);
			*dst++ = itbl[k];
		}
	}
}

/* swap the 3 buffers according to the codec */
static void c47_swap_bufs(struct sanctx *ctx, uint8_t rotcode)
{
	struct sanrt *rt = &ctx->rt;
	if (rotcode) {
		uint8_t *tmp;
		if (rotcode == 2) {
			tmp = rt->buf1;
			rt->buf1 = rt->buf2;
			rt->buf2 = tmp;
		}
		tmp = rt->buf2;
		rt->buf2 = rt->buf0;
		rt->buf0 = tmp;
	}
}

static void codec47_comp1(uint8_t *src, uint8_t *dst_in, uint8_t *itbl, uint16_t w, uint16_t h)
{
	/* input data is i-frame with half width and height. combining 2 pixels
	 * into a 16bit value, one can then use this value as an index into
	 * the interpolation table to get the missing color between 2 pixels.
	 */
	uint8_t *dst, p8, p82;
	uint16_t px;
	int i, j;

	/* start with 2nd row and create every other.  The first 2 pixels in each
	 * row are taken from the source, the next one is interpolated from the
	 * last and the following one.
	 */
	dst = dst_in + w;
	for (i = 0; i < h; i += 2) {
		p8 = *src++;
		*dst++ = p8;
		*dst++ = p8;
		px = p8;
		for (j = 2; j < w; j += 2) {
			p8 = *src++;
			px = (px << 8) | p8;
			*dst++ = itbl[px];
			*dst++ = p8;
		}
		dst += w;
	}

	/* do the rows: the first is a copy of the 2nd line, the missing ones
	 * are interpolated using the pixels of the rows above and below.
	 */
	memcpy(dst_in, dst_in + w, w);
	dst = dst_in + (w * 2);
	for (i = 2; i < h - 1; i += 2) {
		for (j = 0; j < w; j ++) {	/* walk along the full row */
			p8 = *(dst - w);	/* pixel from row above */
			p82 = *(dst + w);	/* pixel from row below */
			px = (p82 << 8) | p8;
			*dst++ = itbl[px];
		}
		dst += w;
	}
}

static uint8_t* codec47_block(struct sanctx *ctx, uint8_t *src, uint8_t *dst,
			      uint8_t *p1, uint8_t *p2, uint16_t w,
			      uint8_t *coltbl, uint16_t size)
{
	uint8_t opc, col[2], c;
	uint16_t i, j;
	int8_t *pglyph;

	opc = *src++;
	if (opc >= 0xF8) {
		switch (opc) {
		case 0xff:
			if (size == 2) {
				*(dst + 0 + 0) = *src++; *(dst + 0 + 1) = *src++;
				*(dst + w + 0) = *src++; *(dst + w + 1) = *src++;
			} else {
				size >>= 1;
				src = codec47_block(ctx, src, dst, p1, p2, w, coltbl, size);
				src = codec47_block(ctx, src, dst + size, p1 + size, p2 + size, w, coltbl, size);
				dst += (size * w);
				p1 += (size * w);
				p2 += (size * w);
				src = codec47_block(ctx, src, dst, p1, p2, w, coltbl, size);
				src = codec47_block(ctx, src, dst + size, p1 + size, p2 + size, w, coltbl, size);
			}
			break;
		case 0xfe:
			c = *src++;
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = c;
			break;
		case 0xfd:
			opc = *src++;
			col[0] = *src++;
			col[1] = *src++;
			pglyph = (size == 8) ? ctx->c47_glyph8x8[opc] : ctx->c47_glyph4x4[opc];
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = col[!*pglyph++];
			break;
		case 0xfc:
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = *(p1 + (i * w) + j);
			break;
		default:
			c = coltbl[opc & 7];
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = c;
		}
	} else {
		const int32_t mvoff = c47_mv[opc][0] + (c47_mv[opc][1] * w);
		for (i = 0; i < size; i++)
			for (j = 0; j < size; j++)
				*(dst + (i * w) + j) = *(p2 + (i * w) + j + mvoff);
	}
	return src;
}

static void codec47_comp2(struct sanctx *ctx, uint8_t *src, uint8_t *dst,
			  uint16_t w, uint16_t h, uint8_t *coltbl)
{
	uint8_t *b1 = ctx->rt.buf1, *b2 = ctx->rt.buf2;
	unsigned int i, j;

	for (j = 0; j < h; j += 8) {
		for (i = 0; i < w; i += 8) {
			src = codec47_block(ctx, src, dst + i, b1 + i, b2 + i, w, coltbl, 8);
		}
		dst += (w * 8);
		b1 += (w * 8);
		b2 += (w * 8);
	}
}

static void codec47_comp5(uint8_t *src, uint8_t *dst, uint32_t left)
{
	uint8_t opc, rlen, col, j;

	while (left) {
		opc = *src++;
		rlen = (opc >> 1) + 1;
		if (rlen > left)
			rlen = left;
		if (opc & 1) {
			col = *src++;
			for (j = 0; j < rlen; j++)
				*dst++ = col;
		} else {
			for (j = 0; j < rlen; j++)
				*dst++ = *src++;
		}
		left -= rlen;
	}
}

static void codec47_itable(struct sanctx *ctx, uint8_t *src)
{
	uint8_t *itbl, *p1, *p2;
	int i, j;

	itbl = ctx->rt.c47ipoltbl;
	for (i = 0; i < 256; i++) {
		p1 = p2 = itbl + i;
		for (j = 256 - i; j; j--) {
			*p1 = *p2 = *src++;
			p1 += 1;
			p2 += 256;
		}
		itbl += 256;
	}
	ctx->rt.have_itable = 1;
}

static int codec47(struct sanctx *ctx, uint8_t *src, uint16_t w, uint16_t h)
{
	uint8_t *insrc = src, *dst, comp, newrot, flag;
	uint32_t decsize;
	uint16_t seq;
	int ret;

	seq =    le16_to_cpu(*(uint16_t *)(src + 0));
	comp =   src[2];
	newrot = src[3];
	flag =   src[4];
	decsize  = le32_to_cpu(ua32(src + 14));	/* decoded (raw frame) size */

	if (seq == 0) {
		ctx->rt.lastseq = -1;
		memset(ctx->rt.buf1, src[12], decsize);
		memset(ctx->rt.buf2, src[13], decsize);
	}
	src += 26;
	if (flag & 1) {
		codec47_itable(ctx, src);
		src += 0x8080;
	}

	ret = 0;
	dst = ctx->rt.buf0;
	switch (comp) {
	case 0:	memcpy(dst, src, w * h); break;
	case 1:	codec47_comp1(src, dst, ctx->rt.c47ipoltbl, w, h); break;
	case 2:	if (seq == (ctx->rt.lastseq + 1)) {
			codec47_comp2(ctx, src, dst, w, h, insrc + 8);
		}
		break;
	case 3:	memcpy(ctx->rt.buf0, ctx->rt.buf2, ctx->rt.fbsize); break;
	case 4:	memcpy(ctx->rt.buf0, ctx->rt.buf1, ctx->rt.fbsize); break;
	case 5:	codec47_comp5(src, dst, decsize); break;
	default: break;
	}

	if (seq == ctx->rt.lastseq + 1)
		c47_swap_bufs(ctx, newrot);

	ctx->rt.lastseq = seq;
	if (seq > 1)
		ctx->rt.can_ipol = 1;

	return ret;
}

/******************************************************************************/

/* scale 4x4 input block to 8x8 output block */
static void c48_4to8(uint8_t *dst, uint8_t *src, uint16_t w)
{
	uint16_t p;
	/* dst is always aligned, so we can do at least 16bit stores */
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 8; j += 2) { /* 1px > 2x2 block */
			p = *src++;
			p = (p << 8) | p; /* 1x2 line */
			*((uint16_t *)(dst + w * 0 + j)) = p;  // 0|0
			*((uint16_t *)(dst + w * 1 + j)) = p;  // 0|1
		}
		dst += w * 2;
	}
}

/* process an 8x8 block */
static uint8_t *c48_block(uint8_t *src, uint8_t *dst, uint8_t *db, uint16_t w)
{
	uint8_t opc, sb[16];
	int16_t mvofs;
	uint32_t ofs;
	int i, j, k, l;

	opc = *src++;
	switch (opc) {
	case 0xFF:	/* 1x1 -> 8x8 block scale */
		for (i = 0; i < 16; i++)
			sb[i] = *src;
		src++;
		c48_4to8(dst, sb, w);
		break;
	case 0xFE:	/* 1x 8x8 copy from deltabuf, 16bit mv from src */
		mvofs = (int16_t)le16_to_cpu(ua16(src)); src += 2;
		for (i = 0; i < 8; i++) {
			ofs = w * i;
			for (k = 0; k < 8; k++)
				*(dst + ofs + k) = *(db + ofs + k + mvofs);
		}
		break;
	case 0xFD:	/* 2x2 -> 8x8 block scale */
		sb[ 5] = *src++;
		sb[ 7] = *src++;
		sb[13] = *src++;
		sb[15] = *src++;

		sb[0] = sb[1] = sb[4] = sb[5];
		sb[2] = sb[3] = sb[6] = sb[7];
		sb[8] = sb[9] = sb[12] = sb[13];
		sb[10] = sb[11] = sb[14] = sb[15];
		c48_4to8(dst, sb, w);
		break;
	case 0xFC:	/* 4x copy 4x4 block, per-block c48_mv, index from source */
		for (i = 0; i < 8; i += 4) {
			for (k = 0; k < 8; k += 4) {
				opc = *src++;
				mvofs = c37_mv[0][opc * 2] + (c37_mv[0][opc * 2 + 1] * w);
				for (j = 0; j < 4; j++) {
					ofs = (w * (j + i)) + k;
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *(db + ofs + l + mvofs);
				}
			}
		}
		break;
	case 0xFB: 	/* Copy 4x 4x4 blocks, per-block mv from source */
		for (i = 0; i < 8; i += 4) {			/* 2 */
			for (k = 0; k < 8; k += 4) {		/* 2 */
				mvofs = le16_to_cpu(ua16(src)); src += 2;
				for (j = 0; j < 4; j++) {	/* 4 */
					ofs = (w * (j + i)) + k;
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *(db + ofs + l + mvofs);
				}
			}
		}
		break;
	case 0xFA:	/* scale 4x4 input block to 8x8 dest block */
		c48_4to8(dst, src, w);
		src += 16;
		break;
	case 0xF9:	/* 16x 2x2 copy from delta, per-block c48_mv */
		for (i = 0; i < 8; i += 2) {				/* 4 */
			for (j = 0; j < 8; j += 2) {			/* 4 */
				ofs = (w * i) + j;
				opc = *src++;
				mvofs = c37_mv[0][opc * 2] + (c37_mv[0][opc * 2 + 1] * w);
				for (l = 0; l < 2; l++) {
					*(dst + ofs + l + 0) = *(db + ofs + l + 0 + mvofs);
					*(dst + ofs + l + w) = *(db + ofs + l + w + mvofs);
				}
			}
		}
		break;
	case 0xF8:	/* 16x 2x2 blocks copy, mv from source */
		for (i = 0; i < 8; i += 2) {				/* 4 */
			for (j = 0; j < 8; j += 2) {			/* 4 */
				ofs = w * i + j;
				mvofs = le16_to_cpu(ua16(src)); src += 2;
				for (l = 0; l < 2; l++) {
					*(dst + ofs + l + 0) = *(db + ofs + l + 0 + mvofs);
					*(dst + ofs + l + w) = *(db + ofs + l + w + mvofs);
				}
			}
		}
		break;
	case 0xF7:	/* copy 8x8 block from src to dest */
		for (i = 0; i < 8; i++) {
			ofs = i * w;
			for (l = 0; l < 8; l++)
				*(dst + ofs + l) = *src++;
		}
		break;
	default:	/* copy 8x8 block from prev, c48_mv */
		mvofs = c37_mv[0][opc * 2] + (c37_mv[0][opc * 2 + 1] * w);
		for (i = 0; i < 8; i++) {
			ofs = i * w;
			for (l = 0; l < 8; l++)
				*(dst + ofs + l) = *(db + ofs + l + mvofs);
		}
		break;
	}
	return src;
}

static void codec48_comp3(uint8_t *src, uint8_t *dst, uint8_t *db,
			  uint8_t *itbl, uint16_t w, uint16_t h)
{
	int i, j;

	for (i = 0; i < h; i += 8) {
		for (j = 0; j < w; j += 8) {
			src = c48_block(src, dst + j, db + j, w);
		}
		dst += w * 8;
		db += w * 8;
	}
}

static int codec48(struct sanctx *ctx, uint8_t *src, uint16_t w, uint16_t h)
{
	uint8_t comp, flag, *dst;
	uint32_t pktsize, decsize;
	uint16_t seq;
	int ret;

	comp =	src[0];		/* subcodec */
	if (src[1] != 1)	/* mvec table variant, always 1 with MotS */
		return 26;

	seq = le16_to_cpu(*(uint16_t*)(src + 2));

	/* decsize is the size of the raw frame aligned to 8x8 blocks
	 * when seq == 0, otherwise it's identical to pktsize, which
	 * indicates the number of bytes in the datastream for this packet.
	 */
	decsize = le32_to_cpu(ua32(src + 4));
	pktsize = le32_to_cpu(ua32(src + 8));
	flag =	src[12];

	if (seq == 0) {
		ctx->rt.lastseq = -1;
		memset(ctx->rt.buf0, 0, decsize);
		memset(ctx->rt.buf2, 0, decsize);
	}

	src += 16;
	if (flag & 8) {
		codec47_itable(ctx, src);
		src += 0x8080;
	}

	ret = 0;
	dst = ctx->rt.buf0;
	switch (comp) {
	case 0:	memcpy(dst, src, pktsize); break;
	case 2: codec47_comp5(src, dst, decsize); break;
	case 3: codec48_comp3(src, dst, ctx->rt.buf2, ctx->rt.c47ipoltbl, w, h); break;
	case 5: codec47_comp1(src, dst, ctx->rt.c47ipoltbl, w, h); break;
	default: break;
	}

	if (seq > 1)
		ctx->rt.can_ipol = 1;
	ctx->rt.lastseq = seq;
	c47_swap_bufs(ctx, 1);	/* swap 0 and 2 */

	return ret;
}

/******************************************************************************/

static void codec37_comp1(uint8_t *src, uint8_t *dst, uint8_t *db, uint16_t w,
			  uint16_t h, uint8_t mvidx)
{
	uint8_t opc, run, skip;
	int32_t mvofs, ofs;
	int i, j, k, l, len;

	run = 0;
	len = -1;
	opc = 0;
	for (i = 0; i < h; i += 4) {
		for (j = 0; j < w; j += 4) {
			if (len < 0) {
				len = (*src) >> 1;
				run = !!((*src++) & 1);
				skip = 0;
			} else {
				skip = run;
			}

			if (!skip) {
				opc = *src++;
				if (opc == 0xff) {
					len--;
					for (k = 0; k < 4; k++) {
						ofs = j + (k * w);
						for (l = 0; l < 4; l++) {
							if (len < 0) {
								len = (*src) >> 1;
								run = !!((*src++) & 1);
								if (run)
									opc = *src++;
							}
							*(dst + ofs + l) = run ? opc : *src++;
							len--;
						}
					}
					continue;
				}
			}
			/* 4x4 block copy from prev with MV */
			mvofs = c37_mv[mvidx][opc*2] + (c37_mv[mvidx][opc*2 + 1] * w);
			for (k = 0; k < 4; k++) {
				ofs = j + (k * w);
				for (l = 0; l < 4; l++)
					*(dst + ofs + l) = *(db + ofs + l + mvofs);
			}
			len -= 1;
		}
		dst += w * 4;
		db += w * 4;
	}
}

static void codec37_comp3(uint8_t *src, uint8_t *dst, uint8_t *db, uint16_t w, uint16_t h,
			  uint8_t mvidx, const uint8_t f4, const uint8_t c4)
{
	uint8_t opc, c, copycnt;
	int32_t ofs, mvofs;
	int i, j, k, l;

	copycnt = 0;
	for (i = 0; i < h; i += 4) {
		for (j = 0; j < w; j += 4) {

			/* copy a 4x4 block from the previous frame from same spot */
			if (copycnt > 0) {
c37_blk:
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++) {
						*(dst + ofs + l) = *(db + ofs + l);
					}
				}
				copycnt--;
				continue;
			}

			opc = *src++;
			if (opc == 0xff) {
				/* 4x4 block, per-pixel data from source */
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *src++;
				}
			} else if (f4 && (opc == 0xfe)) {
				/* 4x4 block, per-line color from source */
				for (k = 0; k < 4; k++) {
					c = *src++;
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = c;
				}
			} else if (f4 && (opc == 0xfd)) {
				/* 4x4 block, per block color from source */
				c = *src++;
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = c;
				}
			} else if (c4 && (opc == 0)) {
				/* copy 4x4 block from prev frame, cnt from src */
				copycnt = 1 + *src++;
				goto c37_blk; /* curr. block needs to be handled too */
			} else {
				/* 4x4 block copy from prev with MV */
				mvofs = c37_mv[mvidx][opc*2] + (c37_mv[mvidx][opc*2 + 1] * w);
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *(db + ofs + l + mvofs);
				}
			}
		}
		dst += w * 4;
		db += w * 4;
	}
}

static int codec37(struct sanctx *ctx, uint8_t *src, uint16_t w, uint16_t h,
		   uint16_t top, uint16_t left)
{
	uint8_t comp, mvidx, flag, *dst, *db;
	uint32_t decsize;
	uint16_t seq;
	int ret;

	comp = src[0];
	mvidx = src[1];
	if (mvidx > 2)
		return 18;
	seq = le16_to_cpu(*(uint16_t *)(src + 2));
	decsize = le32_to_cpu(ua32(src + 4));
	flag = src[12];

	if (comp == 0 || comp == 2) {
		memset(ctx->rt.buf2, 0, decsize);
	}

	/* Codec37's  buffers are private, no other codec must touch them.
	 * Therefore we operate on buf1/buf2 rather than buf0/buf2, since
	 * buf0 is also touched by default by other codecs, and this results
	 * in unexpected image issues.
	 * Unlike c47, buffers need to be pre-rotated.
	 */
	if ((comp == 1 || comp == 3 || comp == 4)
	    && ((seq & 1) || !(flag & 1))) {
		void *tmp = ctx->rt.buf1;
		ctx->rt.buf1 = ctx->rt.buf2;
		ctx->rt.buf2 = tmp;
	}

	src += 16;
	ret = 0;
	dst = ctx->rt.buf1 + (top * w) + left;
	db = ctx->rt.buf2 + (top * w) + left;

	switch (comp) {
	case 0: memcpy(dst, src, decsize); break;
	case 1: codec37_comp1(src, dst, db, w, h, mvidx); break;
	case 2: codec47_comp5(src, dst, decsize); break;
	case 3: /* fallthrough */
	case 4: codec37_comp3(src, dst, db, w, h, mvidx, flag & 4, comp == 4); break;
	default: break;
	}

	/* copy the final image to buf0 in case another codec needs to operate
	 * on it.
	 */
	memcpy(ctx->rt.buf0, ctx->rt.buf1, ctx->rt.fbsize);
	ctx->rt.lastseq = seq;

	return ret;
}

/******************************************************************************/

static void codec1(struct sanctx *ctx, uint8_t *src, uint16_t w, uint16_t h,
		   int16_t top, int16_t left)
{
	uint8_t *dst, code, col;
	uint16_t rlen, dlen;
	int i, j;

	for (i = 0; i < h; i++) {
		dst = ctx->rt.buf0 + ((top + i) * ctx->rt.pitch) + left;
		dlen = le16_to_cpu(ua16(src)); src += 2;
		while (dlen > 0) {
			code = *src++; dlen--;
			rlen = (code >> 1) + 1;
			if (code & 1) {
				col = *src++; dlen--;
				if (col)
					for (j = 0; j < rlen; j++)
						*(dst + j) = col;
				dst += rlen;
			} else {
				for (j = 0; j < rlen; j++) {
					col = *src++;
					if (col)
						*dst = col;
					dst++;
				}
				dlen -= rlen;
			}
		}
	}
}

/******************************************************************************/

static int fobj_alloc_buffers(struct sanrt *rt, uint16_t w, uint16_t h, uint8_t bpp, unsigned align)
{
	uint16_t wb, hb;
	uint32_t bs, fbs;
	uint8_t *b;

	if (align > 1) {
		/* align sizes */
		align -= 1;
		wb = (w + align) & ~align;
		hb = (h + align) & ~align;
	} else {
		wb = w;
		hb = h;
	}

	/* don't support strides different from image width (yet) */
	if (wb != w) {
		return 50;
	}

	/* we require up to 3 buffers the size of the image.
	 * a front buffer + 2 work buffers,  a buffer used to store
	 * the frontbuffer on "STOR" and an 2 intermediate buffers for
	 * interpolated frames (c47/c48 videos only).
	 *
	 * Then we need a "guard band" before and after the buffers for motion
	 * vectors that point outside the defined video area, esp. for codec37
	 * and codec48.  32 lines (max of mvec tables) is enough to get rid of
	 * all tiny artifacts.
	 */
	bs = wb * hb * bpp;		/* block-aligned 8 bit sizes */
	bs = (bs + 0xfff) & ~0xfff;	/* align to 4K */
	fbs = bs * 6 + (wb * 32 * 4);	/* 4 buffers, 4 guard "bands" */
	b = (uint8_t *)malloc(fbs);
	if (!b)
		return 51;
	memset(b, 0, fbs);	/* clear everything including the guard bands */

	if (rt->buf)
		free(rt->buf);

	rt->buf = b;
	rt->buf0 = b + (wb * 32);	/* leave a guard band for motion vectors */
	rt->buf1 = rt->buf0 + (wb * 32) + bs;
	rt->buf2 = rt->buf1 + (wb * 32) + bs;
	rt->buf3 = rt->buf2 + (wb * 32) + bs;
	rt->buf4 = rt->buf3 + bs;
	rt->buf5 = rt->buf4 + bs;
	rt->fbsize = w * h * bpp;	/* image size reported to caller */
	rt->bufw = w;			/* buffer (aligned) width */
	rt->bufh = h;

	return 0;
}

static int handle_FOBJ(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	struct sanrt *rt = &ctx->rt;
	uint16_t w, h, wr, hr, align, param2;
	uint8_t codec, param;
	int16_t left, top;
	int ret;

	codec = src[0];
	param = src[1];

	left = le16_to_cpu(*( int16_t *)(src + 2));
	top  = le16_to_cpu(*( int16_t *)(src + 4));
	w    = le16_to_cpu(*(uint16_t *)(src + 6));
	h    = le16_to_cpu(*(uint16_t *)(src + 8));
	param2 = le16_to_cpu(*(uint16_t *)(src + 12));

	align = (codec == 37) ? 4 : 2;
	align = (codec == 48) ? 8 : align;

	/* ignore nonsensical dimensions in first frames, happens with
	 * some Full Throttle and RA2 videos.
	 */
	if ((w < align) || (h < align) || (w > 640) || (h > 480))
		return 0;

	/* disable left/top for codec37/47/48 videos.  Except for SotE, none use
	 * it, and SotE uses it as a hack to get "widescreen" aspect, i.e. it's
	 * just a black bar of 60 pixels heigth at the top.
	 */
	if (codec == 37 || codec == 47 || codec == 48)
		left = top = 0;

	/* decide on a buffer size */
	if (!rt->have_vdims) {
		/* for ANIMv0/v1 (i.e. Rebel Assault 1) just create a 320x200
		 * buffer.
		 */
		if (rt->version < 2) {
			rt->have_vdims = 1;
			wr = 320;
			hr = 200;
			rt->pitch = 320;
			rt->frmw = 320;
			rt->frmh = 200;
		} else if (codec == 37 || codec == 47 || codec == 48) {
			/* these codecs work on whole frames, trust their dimensions */
			rt->have_vdims = 1;
			wr = w;
			hr = h;
			rt->pitch = w;
			rt->frmw = w;
			rt->frmh = h;
		} else {
			/* don't know (yet) */
			if ((left == 0) && (top == 0) && (w >= 200) && (h >= 100))
				rt->have_vdims = 1; /* *looks* legit */

			wr = w + left;
			hr = h + top;
			rt->pitch = w;
			rt->frmw = w;
			rt->frmh = h;
		}
		ret = fobj_alloc_buffers(rt, wr, hr, 1, align);
		if (ret)
			return ret;
	}

	ret = 0;
	if ((rt->have_vdims && ((w > rt->bufw) || (h > rt->bufh))) || (!rt->fbsize)) {
		ret = fobj_alloc_buffers(rt, w, h, 1, align);
		if (ret != 0)
			return ret;
	}

	/* default image buffer is buf0 */
	rt->vbuf = rt->buf0;

	switch (codec) {
	case 1:
	case 3: codec1(ctx, src + 14, w, h, top, left); break;
	case 37:ret = codec37(ctx, src + 14, w, h, top, left); break;
	case 47:ret = codec47(ctx, src + 14, w, h); break;
	case 48:ret = codec48(ctx, src + 14, w, h); break;
	default: ret = 10;
	}

	if (ret == 0) {
		ctx->rt.have_frame = 1;

		if (rt->version < 2) {
			/* that few stupid Full Throttle videos which are slightly
			 * larger but still have the 320x200 visible area.
			 * Concatenate the visible image area to 320x200 in the
			 * STOR buffer.
			 */
			if (w > 320 && w < 400 && h > 200 && h < 250) {
				int i, j;
				for (i = 0; i < 200; i++)
					for (j = 0; j < 320; j++)
						*(rt->buf3 + (i * 320) + j) = *(rt->vbuf + (i * w) + j);
				rt->vbuf = rt->buf3;
			}
		}
	}

	return ret;
}

static void handle_NPAL(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	read_palette(ctx, src);
}

static inline uint8_t _u8clip(int a)
{
	if (a > 255) return 255;
	else if (a < 0) return 0;
	else return a;
}

static void handle_XPAL(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	const uint16_t cmd = be16_to_cpu(*(uint16_t *)(src + 2));
	uint32_t *pal = ctx->rt.palette;
	int16_t *dp = ctx->rt.deltapal;
	int i, j, t2[3];

	src += 4;

	/* cmd1: apply delta */
	if (cmd == 1) {
		for (i = 0; i < 768; i += 3) {
			t2[0] = (*pal >>  0) & 0xff;
			t2[1] = (*pal >>  8) & 0xff;
			t2[2] = (*pal >> 16) & 0xff;
			for (j = 0; j < 3; j++) {
				int cl = (t2[j] * 129) + le16_to_cpu(*dp++);
				t2[j] = _u8clip(cl / 128) & 0xff;
			}
			*pal++ = 0xff << 24 | t2[2] << 16 | t2[1] << 8 | t2[0];
		}
	/* cmd0/2: read deltapal values/+new palette */
	} else if (cmd == 0 || cmd == 2) {
		memcpy(ctx->rt.deltapal, src, 768 * 2);
		if (size > (768 * 2 + 4))	/* cmd 2 */
			read_palette(ctx, src + (768 * 2));
	}
}

static void iact_audio_scaled(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint8_t v1, v2, v3, *src2, *ib = ctx->rt.iactbuf;
	uint16_t count, len;
	int16_t *dst;

	/* algorithm taken from ScummVM/engines/scumm/smush/smush_player.cpp */
	while (size > 0) {
		if (ctx->rt.iactpos >= 2) {
			len = be16_to_cpu(*(uint16_t *)ib) + 2 - ctx->rt.iactpos;
			if (len > size) {  /* continued in next IACT chunk. */
				memcpy(ib + ctx->rt.iactpos, src, size);
				ctx->rt.iactpos += size;
				size = 0;
			} else {
				memcpy(ib + ctx->rt.iactpos, src, len);
				dst = (int16_t *)ctx->rt.abuf;
				src2 = ib + 2;
				v1 = *src2++;
				v2 = v1 >> 4;
				v1 &= 0x0f;
				count = 1024 * 2;
				do {
					v3 = *src2++;
					if (v3 == 0x80) {
						*dst++ = cpu_to_le16(src2[0] << 8 | src2[1]);
						src2 += 2;
					} else {
						*dst++ = cpu_to_le16((int8_t)v3) << ((count & 1) ? v1 : v2);
					}
				} while (--count);
				ctx->io->queue_audio(ctx->io->userctx, ctx->rt.abuf, SZ_AUDIOOUT);
				size -= len;
				src += len;
				ctx->rt.iactpos = 0;
			}
		} else {
			if (size > 1 && ctx->rt.iactpos == 0) {
				*ib = *src++;
				ctx->rt.iactpos++;
				size--;
			}
			*(ib + ctx->rt.iactpos) = *src++;
			ctx->rt.iactpos++;
			size--;
		}
	}
}

static void atrk_put_sample(struct sanatrk *atrk, int16_t samp)
{
	int16_t *dst;

	if (atrk->wrptr >= ATRK_MAXWP)
		atrk->wrptr = 0;

	dst = (int16_t *)(atrk->data + atrk->wrptr);
	*dst = samp;
	atrk->wrptr += 2;
	atrk->datacnt += 2;
}

/* read PCM source data, and convert it to 16bit/stereo/22kHz samplerate
 * if necessary, to match the format of the IACT tracks of codec47/48 videos.
 * FIXME: the "upsampling" is just doubling the samples, so super crude; best
 * to interpolate between last generated and newly generated sample.
 */
static void aud_read_pcmsrc(struct sanctx *ctx, struct sanatrk *atrk,
			    uint32_t size, uint8_t *src)
{
	uint32_t space, toend;
	int16_t s1, s2, *dst;
	uint8_t v[3];
	int f, i, fast;

	f = atrk->flags;
	if (!f & AUD_INUSE)
		return;

	/* calculate expected space requirements to determine if the wrptr
	 * may wrap around, or the fast path can be used.
	 */
	space = size;
	if (f & AUD_SRC12BIT)
		space = (size + atrk->pdcnt) / 3 * 4;
	else if (f & AUD_SRC8BIT)
		space = size * 2;

	if (f & AUD_1CH)
		space *= 2;
	if (f & AUD_RATE11KHZ)
		space *= 2;

	toend = ATRK_MAXWP - atrk->wrptr;
	fast = space <= toend;
	dst = (int16_t *)(atrk->data + atrk->wrptr);

	if (0 == (f & (AUD_SRC12BIT | AUD_SRC8BIT | AUD_RATE11KHZ | AUD_1CH))) {
		/* optimal case: 22kHz/16bit/2ch source */
		/* handle wraparound of wrptr */
		if (size > toend) {
			memcpy(atrk->data + atrk->wrptr, src, toend);
			memcpy(atrk->data, src + toend, size - toend);
		} else {
			memcpy(atrk->data + atrk->wrptr, src, size);
		}
		atrk->datacnt += size;
		size = 0;
	} else if (f & AUD_SRC12BIT) {
		/* 12->16bit to stereo decode, need at least 3 bytes */
		while (size > 2 || atrk->pdcnt) {
			for (i = 0; i < 3; i++) {
				if (atrk->pdcnt > 0) {
					v[i] = atrk->pd[i];
					atrk->pdcnt--;
				} else {
					v[i] = *src++;
					size--;
				}
			}
			s1 = ((((v[1] & 0x0f) << 8) | v[0]) << 4) - 0x8000;
			s2 = ((((v[1] & 0xf0) << 4) | v[2]) << 4) - 0x8000;
			if (f & AUD_1CH) {
				if (fast) {
					*dst++ = s1;
					*dst++ = s1;
					*dst++ = s2;
					*dst++ = s2;
				} else {
					atrk_put_sample(atrk, s1);
					atrk_put_sample(atrk, s1);
					atrk_put_sample(atrk, s2);
					atrk_put_sample(atrk, s2);
				}
				if (f & AUD_RATE11KHZ) {
					if (fast) {
						*dst++ = s1;
						*dst++ = s1;
						*dst++ = s2;
						*dst++ = s2;
					} else {
						atrk_put_sample(atrk, s1);
						atrk_put_sample(atrk, s1);
						atrk_put_sample(atrk, s2);
						atrk_put_sample(atrk, s2);
					}
				}
			} else {
				if (fast) {
					*dst++ = s1;
					*dst++ = s2;
				} else {
					atrk_put_sample(atrk, s1);
					atrk_put_sample(atrk, s2);
				}
				if (f & AUD_RATE11KHZ) {
					if (fast) {
						*dst++ = s1;
						*dst++ = s2;
					} else {
						atrk_put_sample(atrk, s1);
						atrk_put_sample(atrk, s2);
					}
				}
			}
		}

	} else if (f & AUD_SRC8BIT) {
		/* 8 -> 16 bit to stereo conversion */
		while (size > 0) {
			s1 = ((*src++) << 8) ^ 0x8000;
			size--;

			if (f & AUD_1CH) {
				if (fast) {
					*dst++ = s1;
					*dst++ = s1;
				} else {
					atrk_put_sample(atrk, s1); /* L */
					atrk_put_sample(atrk, s1);	/* R */
				}
				if (f & AUD_RATE11KHZ) {
					if (fast) {
						*dst++ = s1;
						*dst++ = s1;
					} else {
						atrk_put_sample(atrk, s1);
						atrk_put_sample(atrk, s1);
					}
				}
			} else {
				if (fast)
					*dst++ = s1;
				else
					atrk_put_sample(atrk, s1);
				if (f & AUD_RATE11KHZ) {
					if (fast)
						*dst++ = s1;
					else
						atrk_put_sample(atrk, s1);
				}
			}

		}

	} else {
		/* 16bit mono samples -> 16bit stereo conversion. */

		/* construct a 16 bit sample from optional leftover byte from
		 * the last iteration
		 */
		s1 = 0;
		if (atrk->pdcnt) {
			s1 |= atrk->pd[0];
			atrk->pdcnt--;
			if (size > 0) {
				s1 = (s1 << 8) | *src++;
				size--;
			} else {
				/* zero new data, bail */
				return;
			}
			if (fast) {
				*dst++ = s1;
				*dst++ = s1;
			} else {
				atrk_put_sample(atrk, s1);	/* L */
				atrk_put_sample(atrk, s1);	/* R */
			}
			if (f & AUD_RATE11KHZ) {
				if (fast) {
					*dst++ = s1;
					*dst++ = s1;
				} else {
					atrk_put_sample(atrk, s1);
					atrk_put_sample(atrk, s1);
				}
			}
		}

		while (size > 1) {
			s1 = *(int16_t *)src;
			if (fast) {
				*dst++ = s1;
				*dst++ = s1;
			} else {
				atrk_put_sample(atrk, s1);	/* L */
				atrk_put_sample(atrk, s1);	/* R */
			}
			if (f & AUD_RATE11KHZ) {
				if (fast) {
					*dst++ = s1;
					*dst++ = s1;
				} else {
					atrk_put_sample(atrk, s1);
					atrk_put_sample(atrk, s1);
				}
			}
			src += 2;
			size -= 2;
		}
	}

	/* sometimes not all the data can be consumed at once (12bit!):
	 * store the rest for the next datapacket for this track
	 */
	atrk->pdcnt = size;
	for (i = 0; i < size; i++)
		atrk->pd[i] = *src++;

	/* update track pointers, slowpath does that in atrk_put_sample() */
	if (fast) {
		atrk->wrptr += space;
		if (atrk->wrptr >= ATRK_MAXWP)
			atrk->wrptr -= ATRK_MAXWP;
		atrk->datacnt += space;
	}
}

static void aud_mixs16(uint8_t *ds1, uint8_t *s1, uint8_t *s2, int bytes)
{
	int16_t *src1 = (int16_t *)s1;
	int16_t *src2 = (int16_t *)s2;
	int16_t *dst = (int16_t *)ds1;
	int d1, d2, d3;

	while (bytes > 1) {
		d1 = (*src1++) + 32768;	/* s16 sample to u16 */
		d2 = (*src2++) + 32768;
		bytes -= 2;

		if (d1 < 32768 && d2 < 32768) {
			d3 = (d1 * d2) / 32768;
		} else {
			d3 = (2 * (d1 + d2)) - ((d1 * d2) / 32768) - 65536;
		}

		*dst++ = (d3 - 32768);	/* mixed u16 back to s16 and write */
	}
}

static struct sanatrk *aud_find_trk(struct sanctx *ctx, uint16_t trkid, int fail)
{
	struct sanrt *rt = &ctx->rt;
	struct sanatrk *atrk;
	int i, newid;

	newid = -1;
	for (i = 0; i < ATRK_NUM; i++) {
		atrk = &(rt->sanatrk[i]);
		if ((atrk->flags & AUD_INUSE) && (trkid == atrk->trkid))
			return atrk;
		if ((newid < 0) && (0 == (atrk->flags & AUD_INUSE)))
			newid = i;
	}
	if ((newid > -1) && (!fail)) {
		atrk = &(rt->sanatrk[newid]);
		atrk->rdptr = 0;
		atrk->wrptr = 0;
		atrk->datacnt = 0;
		atrk->flags = 0;
		atrk->pdcnt = 0;
		atrk->trkid = trkid;
		atrk->dataleft = 0;
		return atrk;
	}
	return NULL;
}

/* buffer data from iMUSE IACT block.
 * the source can be multiple independent tracks, all with different rates,
 * channels, resolution.  Decode the data and if necessary, convert it to
 * 22.05kHz, 16bit, stereo.  Final mixing of the tracks is done at the end
 * of FRME handling.
 */
static void iact_buffer_imuse(struct sanctx *ctx, uint32_t size, uint8_t *src,
			      uint16_t trkid, uint16_t uid, int done)
{
	uint32_t cid, csz, mapsz;
	uint16_t rate, bits, chnl;
	struct sanatrk *atrk;

	if (uid == 1)
		trkid += 100;
	else if (uid == 2)
		trkid += 200;
	else if (uid == 3)
		trkid += 300;
	else if ((uid >= 100) && (uid <= 163))
		trkid += 400;
	else if ((uid >= 200) && (uid <= 263))
		trkid += 500;
	else if ((uid >= 300) && (uid <= 363))
		trkid += 600;

	atrk = aud_find_trk(ctx, trkid, 0);
	if (!atrk)
		return;

	/*
	 * read header of new track. NOTE: subchunks aren't 16bit aligned!
	 */
	if (0 == (atrk->flags & AUD_INUSE)) {
		if (size < 24)
			return;
		cid = le32_to_cpu(ua32(src + 0));
		if (cid != iMUS)
			return;

		cid = le32_to_cpu(ua32(src + 8));
		mapsz = be32_to_cpu(ua32(src + 12));

		size -= 16;
		src += 16;

		if (cid != MAP_ || mapsz > size)
			return;

		/* the MAP_ chunk again has a few subchuks, need the FRMT tag */
		while (mapsz > 7 && size > 7) {
			cid = le32_to_cpu(ua32(src + 0));
			csz = be32_to_cpu(ua32(src + 4));

			size -= 8;
			mapsz -= 8;
			src += 8;

			if (cid == FRMT) {
				bits = be16_to_cpu(ua16(src + 10));
				rate = be16_to_cpu(ua16(src + 14));
				chnl = be16_to_cpu(ua16(src + 18));
				if (bits == 8)
					atrk->flags |= AUD_SRC8BIT;
				else if (bits == 12)
					atrk->flags |= AUD_SRC12BIT;
				if (rate == 11025)
					atrk->flags |= AUD_RATE11KHZ;
				if (chnl == 1)
					atrk->flags |= AUD_1CH;
			}

			src += csz;
			size -= csz;
			mapsz -= csz;
		}

		/* now there should be "DATA" with the TOTAL len of sound of the whole track */
		if (size < 8)
			return;
		cid = le32_to_cpu(ua32(src + 0));
		src += 8;
		size -= 8;
		if (cid != DATA)
			return;

		atrk->flags |= AUD_INUSE;	/* active track */
		ctx->rt.acttrks++;
	}

	if (done)
		atrk->flags |= AUD_SRCDONE;	/* free track after mixing */

	aud_read_pcmsrc(ctx, atrk, size, src);
}

/* count all active tracks: tracks which are still allocated */
static int aud_count_active(struct sanrt *rt)
{
	struct sanatrk *atrk;
	int i, active;

	active = 0;
	for (i = 0; i < ATRK_NUM; i++) {
		atrk = &(rt->sanatrk[i]);
		if (AUD_INUSE == (atrk->flags & (AUD_INUSE)))
			active++;
	}
	return active;
}

/* count all mixable tracks, with shortest buffer size returned too */
static int aud_count_mixable(struct sanrt *rt, uint32_t *minlen)
{
	struct sanatrk *atrk;
	int i, mixable;
	uint32_t ml;

	ml = ~0;
	mixable = 0;
	for (i = 0; i < ATRK_NUM; i++) {
		atrk = &(rt->sanatrk[i]);
		if ((AUD_INUSE == (atrk->flags & (AUD_INUSE | AUD_MIXED)))
		    && (atrk->datacnt > 1)) {
			mixable++;
			if (ml > atrk->datacnt)
				ml = atrk->datacnt;
		}
	}

	*minlen = ml;
	return mixable;
}

/* clear the AUD_MIXED flag from all tracks */
static void aud_reset_mixable(struct sanrt *rt)
{
	int i;

	for (i = 0; i < ATRK_NUM; i++)
		rt->sanatrk[i].flags &= ~AUD_MIXED;
}

/* get the FIRST mixable track. */
static struct sanatrk *aud_get_mixable(struct sanrt *rt)
{
	struct sanatrk *atrk;
	int i;
	for (i = 0; i < ATRK_NUM; i++) {
		atrk = &(rt->sanatrk[i]);
		if ((AUD_INUSE == (atrk->flags & (AUD_INUSE | AUD_MIXED)))
			&& (atrk->datacnt > 1))
			return atrk;
	}
	return NULL;
}

static void aud_atrk_consume(struct sanrt *rt, struct sanatrk *atrk, uint32_t bytes)
{
	atrk->datacnt -= bytes;
	atrk->rdptr += bytes;

	if (atrk->datacnt == 0) {
		atrk->rdptr = 0;
		atrk->wrptr = 0;
		if (atrk->flags & AUD_SRCDONE) {
			atrk->flags = 0;
			atrk->pdcnt = 0;
			atrk->trkid = 0;
			rt->acttrks--;
		}
	} else {
		/* move rest to front of buffer to avoid wraparound */
		/* FIXME FIXME fix the ring buffer instead, this is EXPENSIVE */
		if (atrk->rdptr >= atrk->datacnt) {
			memcpy(atrk->data, atrk->data + atrk->rdptr, atrk->datacnt);
			atrk->rdptr = 0;
			atrk->wrptr = atrk->datacnt;
		}
	}
}

/* mix all of the tracks which have data available together:
 * (1) find all ready tracks with data
 * (2) find out which one hast the _least_ available (minlen)
 * (3) of all tracks with data, take minlen bytes and mix them into the dest buffer
 * (4) have any of these tracks finished (i.e. no more data incoming)
 *	yes -> go back to (1)
 *	no -> we're done, queue audio buffer, wait for next frame with more
 *		track data.
 */
static void aud_mix_tracks(struct sanctx *ctx)
{
	struct sanrt *rt = &ctx->rt;
	struct sanatrk *atrk1, *atrk2;
	int active1, active2, mixable, step, l3;
	uint32_t minlen1, dstlen, ml2, toend1, toend2, todo;
	uint8_t *dstptr, *src1, *src2, *aptr;

	dstlen = 0;
	step = 0;
	aptr = rt->atmpbuf1;

_aud_mix_again:
	dstptr = rt->atmpbuf1 + dstlen;
	mixable = aud_count_mixable(rt, &minlen1);
	active1 = aud_count_active(rt);

	/* only one mixable track found.  If we haven't mixed before, we can
	 * queue the track buffer directly; otherwise we append to the dest
	 * buffer.
	 */
	if ((mixable == 1) && (minlen1 != ~0)){
		atrk1 = aud_get_mixable(rt);
		if (!atrk1)
			return;

		if (step == 0) {
			aptr = atrk1->data + atrk1->rdptr;
			dstlen += atrk1->datacnt;
			aud_atrk_consume(rt, atrk1, atrk1->datacnt);
		} else {
			toend1 = ATRK_MAXWP - atrk1->rdptr;
			if (toend1 <= minlen1) {
				memcpy(dstptr, atrk1->data + atrk1->rdptr, minlen1);
				aud_atrk_consume(rt, atrk1, minlen1);
			} else {
				todo = minlen1;
				memcpy(dstptr, atrk1->data + atrk1->rdptr, toend1);
				aud_atrk_consume(rt, atrk1, toend1);
				todo -= toend1;
				memcpy(dstptr + toend1, atrk1->data + atrk1->rdptr, todo);
				aud_atrk_consume(rt, atrk1, todo);
			}
			dstlen += minlen1;
		}
	} else if ((mixable != 0) && (minlen1 != ~0)) {
		/* step 0: mix the first 2 tracks together */
		atrk1 = aud_get_mixable(rt);
		atrk1->flags |= AUD_MIXED;
		atrk2 = aud_get_mixable(rt);
		atrk2->flags |= AUD_MIXED;

		src1 = atrk1->data + atrk1->rdptr;
		src2 = atrk2->data + atrk2->rdptr;
		toend1 = ATRK_MAXWP - atrk1->rdptr;
		toend2 = ATRK_MAXWP - atrk2->rdptr;
		if (minlen1 <= toend1 && minlen1 <= toend2) {
			/* best case: both tracks don't wrap around */
			aud_mixs16(dstptr, src1, src2, minlen1);
			aud_atrk_consume(rt, atrk1, minlen1);
			aud_atrk_consume(rt, atrk2, minlen1);
		} else {
			/* reading 'minlen' bytes would wrap one or both around */
			todo = minlen1;
			/* read till the first wraps around */
			ml2 = _min(toend1, toend2);
			aud_mixs16(dstptr, src1, src2, ml2);
			aud_atrk_consume(rt, atrk1, ml2);
			aud_atrk_consume(rt, atrk2, ml2);
			src1 = atrk1->data + atrk1->rdptr;
			src2 = atrk2->data + atrk2->rdptr;
			toend1 -= ml2;
			toend2 -= ml2;
			todo -= ml2;
			/* one toendX is now zero and wrapped around, read the
			 * other until wrap around */
			l3 = _max(toend1, toend2);
			aud_mixs16(dstptr + ml2, src1, src2, l3);
			aud_atrk_consume(rt, atrk1, l3);
			aud_atrk_consume(rt, atrk2, l3);
			src1 = atrk1->data + atrk1->rdptr;
			src2 = atrk2->data + atrk2->rdptr;
			todo -= l3;
			/* now the rest */
			aud_mixs16(dstptr + ml2 + l3, src1, src2, todo);
			aud_atrk_consume(rt, atrk1, todo);
			aud_atrk_consume(rt, atrk2, todo);
		}

		mixable -= 2;

		/* step 1: mix the next tracks into dstptr, minlen is still
		 * in effect.
		 */
		while (mixable > 0) {
			atrk1 = aud_get_mixable(rt);
			if (!atrk1) {
				mixable = 0;
				break;
			}
			atrk1->flags |= AUD_MIXED;
			src1 = atrk1->data + atrk1->rdptr;
			src2 = dstptr;
			toend1 = ATRK_MAXWP - atrk1->rdptr;
			if (minlen1 <= toend1) {
				aud_mixs16(dstptr, src1, src2, minlen1);
				aud_atrk_consume(rt, atrk1, minlen1);
			} else {
				todo = minlen1;
				aud_mixs16(dstptr, src1, src2, toend1);
				aud_atrk_consume(rt, atrk1, toend1);
				todo -= toend1;
				src1 = atrk1->data + atrk1->rdptr;
				aud_mixs16(dstptr + toend1, src1, src2 + toend1, todo);
				aud_atrk_consume(rt, atrk1, todo);
			}
			--mixable;
		}

		dstlen += minlen1;
		/* now all tracks including the one with minimal lenght have
		 * been mixed into destination.
		 * Now test if the number of active streams has changed.
		 * If not, we're done.  If YES: that means that the track with
		 *  minlen has finished, and we can assume a new minlen, and
		 * repeat this cycle, appending to dst.
		 */
		active2 = aud_count_active(rt);
		if (active2 < active1 && active2 != 0) {
			/* last minlen stream is finished, a new minlen
			 * can be set and more mixing be done, need to
			 * clear the MIXED flag from all active streams.
			 */
			aud_reset_mixable(rt);
			goto _aud_mix_again;
		}
	}
	aud_reset_mixable(rt);
	if (dstlen)
		ctx->io->queue_audio(ctx->io->userctx, aptr, dstlen);
}

static void handle_IACT(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint16_t p[7];
	int i;

	for (i = 0; i < 7; i++)
		p[i] = le16_to_cpu(*(uint16_t*)(src + (i<<1)));

	if (p[0] == 8 && p[1] == 46) {
		if (p[3] == 0) {
			/* subchunkless scaled IACT audio codec47/48 videos */
			iact_audio_scaled(ctx, size - 18, src + 18);
		} else {
			/* imuse-type */
			iact_buffer_imuse(ctx, size - 18, src + 18, p[4], p[3], (p[6] - p[5]) < 2);
		}
	}
}

static int handle_SAUD(struct sanctx *ctx, uint32_t size, uint8_t *src,
			uint32_t saudsize, uint32_t tid)
{
	uint32_t cid, csz, xsize = _min(size, saudsize);
	uint16_t rate;
	struct sanatrk *atrk;

	atrk = aud_find_trk(ctx, tid, 0);
	if (!atrk)
		return 20;

	rate = ctx->rt.samplerate;
	atrk->flags = AUD_SRC8BIT | AUD_1CH;
	while (xsize > 7) {
		cid = le32_to_cpu(ua32(src + 0));
		csz = be32_to_cpu(ua32(src + 4));
		src += 8;
		xsize -= 8;
		size -= 8;
		if (cid == STRK) {
			if (csz >= 14)
				rate = be16_to_cpu(*(uint16_t *)(src + 12));

			if (rate == 22050 || rate == 22222)
				atrk->flags &= ~AUD_RATE11KHZ;
			else	/* HACK ALERT FIXME: rate conversion required */
				atrk->flags |= AUD_RATE11KHZ;

		} else if (cid == SDAT) {
			atrk->flags |= AUD_INUSE;
			atrk->dataleft = csz;
			ctx->rt.acttrks++;
			break;
		}
		src += csz;
		xsize -= csz;
		size -= csz;
	}
	if (size > 0) {
		aud_read_pcmsrc(ctx, atrk, size, src);
		atrk->dataleft -= size;
		if (atrk->dataleft < 0)
			atrk->flags |= AUD_SRCDONE;
	}
	return 0;
}

static int handle_PSAD(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	struct sanrt *rt = &ctx->rt;
	uint32_t t1, csz, tid, idx;
	struct sanatrk *atrk;
	int ret;

	ret = 0;
	/* scummvm says there are 2 type of psad headers, the old one has
	 * all zeroes at data offset 4 at index 0 (which is hopefully the first
	 * ever PSAD block encoutered).
	 */
	if (rt->psadhdr < 1) {
		t1 = ua32(src + 4);
		rt->psadhdr = (t1 == 0) ? 1 : 2;
	}

	if (rt->psadhdr == 1) {
		tid = be32_to_cpu(ua32(src + 0));
		idx = be32_to_cpu(ua32(src + 4));
		src += 12;
		size -= 12;
	} else {
		tid = le16_to_cpu(*(uint16_t*)(src + 0));
		idx = le16_to_cpu(*(uint16_t*)(src + 2));
		src += 10;
		size -= 10;
	}

	if (idx == 0) {
		t1 = le32_to_cpu(ua32(src + 0));
		csz = be32_to_cpu(ua32(src + 4));
		if (t1 == SAUD) {
			ret = handle_SAUD(ctx, size - 8, src + 8, csz, tid);
			if (ret)
				return ret;
		}
	} else {
		/* handle_SAUD should have allocated it */
		atrk = aud_find_trk(ctx, tid, 1);
		if (!atrk)
			return 22;

		aud_read_pcmsrc(ctx, atrk, size, src);
		atrk->dataleft -= size;
		if (atrk->dataleft < 1)
			atrk->flags |= AUD_SRCDONE;
	}
	return 0;
}

/* subtitles: index of message in the Outlaws LOCAL.MSG file, 10000 - 12001.
 * As long as subid is set to non-zero, the subtitle needs to be overlaid
 * over the image.  The chunk also provides hints about where to place
 * the subtitle, which we ignore here.
 */
static void handle_TRES(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint16_t *tres = (uint16_t *)src;
	ctx->rt.subid = size >= 10 ? le16_to_cpu(tres[8]) : 0;
}

static void handle_STOR(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	ctx->rt.to_store = 1;
}

static void handle_FTCH(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	int32_t xoff, yoff, rx, ry;
	uint8_t *db, *dst;
	int i, j;

	if (size != 12) {
		xoff = *(int16_t *)(src + 2);
		yoff = *(int16_t *)(src + 4);
	} else {
		xoff = be32_to_cpu(ua32(src + 4));
		yoff = be32_to_cpu(ua32(src + 8));
	}

	if (ctx->rt.buf0) {
		if (xoff == 0 && yoff == 0)
			memcpy(ctx->rt.buf0, ctx->rt.buf3, ctx->rt.fbsize);
		else {
			db = ctx->rt.buf3;
			dst = ctx->rt.buf0;
			for (i = 0; i < ctx->rt.bufh; i++) {
				ry = (yoff + i) * ctx->rt.pitch;
				if (ry < 0 || ry > ctx->rt.bufh)
					continue;
				for (j = 0; j < ctx->rt.bufw; j++) {
					rx = xoff + j;
					if (rx < 0 || rx > ctx->rt.bufw)
						continue;
					*(dst + ry + rx) = *(db + i * ctx->rt.pitch + j);
				}
			}
		}
		ctx->rt.have_frame = 1;
	}
}

static int handle_FRME(struct sanctx *ctx, uint32_t size)
{
	struct sanrt *rt = &ctx->rt;
	uint32_t cid, csz;
	uint8_t *src;
	int ret;

	ret = allocfrme(ctx, size);
	if (ret)
		return ret;

	src = rt->fcache;
	if (read_source(ctx, src, size))
		return 10;

	ret = 0;
	while ((size > 7) && (ret == 0)) {
		cid = le32_to_cpu(ua32(src + 0));
		csz = be32_to_cpu(ua32(src + 4));

		src += 8;
		size -= 8;

		if (csz > size)
			return 17;

		switch (cid)
		{
		case NPAL: handle_NPAL(ctx, csz, src); break;
		case FOBJ: ret = handle_FOBJ(ctx, csz, src); break;
		case IACT: handle_IACT(ctx, csz, src); break;
		case TRES: handle_TRES(ctx, csz, src); break;
		case STOR: handle_STOR(ctx, csz, src); break;
		case FTCH: handle_FTCH(ctx, csz, src); break;
		case XPAL: handle_XPAL(ctx, csz, src); break;
		case PSAD: ret = handle_PSAD(ctx, csz, src); break;
		default:   ret = 0;     /* unknown chunk, ignore */
		}
		/* all objects in the SAN stream are padded so their length
		 * is even. */
		if (csz & 1)
			csz += 1;
		src += csz;
		size -= csz;
	}

	/* OK case: all usable bytes of the FRME read, no errors */
	if (ret == 0) {
		if (ctx->rt.have_frame) {
			if (rt->to_store)	/* STOR */
				memcpy(rt->buf3, rt->vbuf, rt->fbsize);

			/* if possible, interpolate a frame using the itable,
			 * and queue that plus the decoded one.
			 */
			if (ctx->io->flags & SANDEC_FLAG_DO_FRAME_INTERPOLATION
			    && rt->have_itable
			    && rt->can_ipol) {
				interpolate_frame(rt->buf5, rt->buf4, rt->vbuf,
						  rt->c47ipoltbl, rt->bufw, rt->bufh);
				rt->have_ipframe = 1;
				rt->can_ipol = 0;
				memcpy(rt->buf4, rt->vbuf, rt->fbsize);
				ctx->io->queue_video(ctx->io->userctx, rt->buf5, rt->fbsize,
					     rt->frmw, rt->frmh, rt->palette,
					     rt->subid, rt->framedur / 2);
			} else {
				ctx->io->queue_video(ctx->io->userctx, rt->vbuf, rt->fbsize,
					     rt->frmw, rt->frmh, rt->palette,
					     rt->subid, rt->framedur);
				/* save frame as possible interpolation source */
				if (rt->have_itable)
					memcpy(rt->buf4, rt->vbuf, rt->fbsize);
			}
		}

		/* mix multi-track audio and queue it up */
		if (rt->acttrks)
			aud_mix_tracks(ctx);

		rt->to_store = 0;
		rt->currframe++;
		rt->subid = 0;
		rt->have_frame = 0;
	}

	return ret;
}

static int handle_AHDR(struct sanctx *ctx, uint32_t size)
{
	struct sanrt *rt = &ctx->rt;
	uint8_t *ahbuf, *xbuf = NULL;
	uint32_t maxframe;
	int ret = SANDEC_OK;

	ahbuf = (uint8_t *)malloc(size);
	if (!ahbuf)
		return 5;
	if (read_source(ctx, ahbuf, size)) {
		ret = 6;
		goto out;
	}

	rt->version = le16_to_cpu(*(uint16_t *)(ahbuf + 0));
	rt->FRMEcnt = le16_to_cpu(*(uint16_t *)(ahbuf + 2));

	/* allocate memory for static work buffers */
	xbuf = malloc(SZ_ALL);
	if (!xbuf) {
		ret = 8;
		goto out;
	}

	rt->iactbuf = (uint8_t *)xbuf;
	rt->palette = (uint32_t *)((uint8_t *)(rt->iactbuf) + SZ_IACT);
	rt->deltapal = (int16_t *)((uint8_t *)rt->palette + SZ_PAL);
	rt->c47ipoltbl = (uint8_t *)rt->deltapal + SZ_DELTAPAL;
	rt->abuf = (uint8_t *)rt->c47ipoltbl + SZ_C47IPTBL;
	rt->atmpbuf1 = rt->abuf + SZ_AUDIOOUT;

	read_palette(ctx, ahbuf + 6);	/* 768 bytes */

	if (rt->version > 1) {
		rt->framedur  =  le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 0));
		rt->framedur = 1000000 / rt->framedur;
		maxframe =       le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 4));
		rt->samplerate = le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 8));

		/* "maxframe" indicates the maximum size of one FRME object
		 * including chunk ID and chunk size in the stream (usually the first)
		 * plus 1 byte.
		 */
		if ((maxframe > 9) && (maxframe < 4 * 1024 * 1024)) {
			ret = allocfrme(ctx, maxframe);
		}
	} else {
		rt->framedur = 1000000 / 10;	/* ANIMv1 default */
		rt->samplerate = 11025;		/* ANIMv1 default */
		rt->frmebufsz = 0;
	}

out:
	if ((ret != SANDEC_OK) && xbuf)
		free(xbuf);

	free(ahbuf);
	return ret;
}

static void sandec_free_memories(struct sanctx *ctx)
{
	/* delete existing FRME buffer */
	if (ctx->rt.fcache)
		free(ctx->rt.fcache);
	/* delete work buffers */
	if (ctx->rt.iactbuf)
		free(ctx->rt.iactbuf);
	/* delete an existing framebuffer */
	if (ctx->rt.buf && ctx->rt.fbsize)
		free(ctx->rt.buf);
	memset(&ctx->rt, 0, sizeof(struct sanrt));
}

/******************************************************************************/
/* public interface */

int sandec_decode_next_frame(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	uint32_t c[2];
	int ret;

	if (!ctx)
		return 1;
	/* in case of previous error, don't continue, just return it again */
	if (ctx->errdone)
		return ctx->errdone;

	/* interpolated frame: was queued first, now queue the decoded one */
	if (ctx->rt.have_ipframe) {
		struct sanrt *rt = &ctx->rt;
		rt->have_ipframe = 0;
		ctx->io->queue_video(ctx->io->userctx, rt->vbuf, rt->fbsize,
				     rt->frmw, rt->frmh, rt->palette,
				     rt->subid, rt->framedur / 2);
		return SANDEC_OK;
	}

	ret = read_source(ctx, c, 8);
	if (ret) {
		if (ctx->rt.currframe == ctx->rt.FRMEcnt)
			ret = SANDEC_DONE;	/* seems we reached file end */
		goto out;
	}

	c[1] = be32_to_cpu(c[1]);
	switch (c[0]) {
	case FRME: 	ret = handle_FRME(ctx, c[1]); break;
	default:	ret = 4;
	}

out:
	ctx->errdone = ret;
	return ret;
}

int sandec_init(void **ctxout)
{
	struct sanctx *ctx;

	if (!ctxout)
		return 1;
	ctx = (struct sanctx *)malloc(sizeof(struct sanctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(struct sanctx));
	/* set to error state initially until a valid file has been opened */
	ctx->errdone = 44;

	c47_make_glyphs(ctx->c47_glyph4x4[0], c47_glyph4_x, c47_glyph4_y, 4);
	c47_make_glyphs(ctx->c47_glyph8x8[0], c47_glyph8_x, c47_glyph8_y, 8);
	*ctxout = ctx;

	return 0;
}

int sandec_open(void *sanctx, struct sanio *io)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	int ret, have_anim = 0, have_ahdr = 0;
	uint32_t c[2];

	if (!io || !sanctx) {
		ret = 1;
		goto out;
	}
	ctx->io = io;

	sandec_free_memories(ctx);

	while (1) {
		ret = read_source(ctx, &c[0], 4 * 2);
		if (ret) {
			ret = 3;
			goto out;
		}
		if (!have_anim) {
			if (c[0] == ANIM) {
				have_anim = 1;
			}
			continue;
		}
		if (c[0] == AHDR) {
			have_ahdr = 1;
			break;
		}
	}

	if (have_ahdr)
		ret = handle_AHDR(ctx, be32_to_cpu(c[1]));
out:
	ctx->errdone = ret;
	return ret;
}

void sandec_exit(void **sanctx)
{
	struct sanctx *ctx;

	if (!sanctx)
		return;
	ctx = *(struct sanctx **)sanctx;
	if (!ctx)
		return;

	sandec_free_memories(ctx);
	free(ctx);
	*sanctx = NULL;
}

int sandec_get_framecount(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.FRMEcnt : 0;
}

int sandec_get_currframe(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.currframe : 0;
}
