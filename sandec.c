/*
 * A/V decoder for LucasArts SMUSH ANM/NUT/SAN/SNM files.
 *
 * Written in 2024-2025 by Manuel Lauss <manuel.lauss@gmail.com>
 *
 * Some codec algorithms (Video, Audio, Palette) liberally taken
 *  from FFmpeg, ScummVM and smushplay projects:
 *  https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/HEAD:/libavcodec/sanm.c
 *  https://github.com/scummvm/scummvm/blob/master/engines/scumm/smush/smush_player.cpp
 *  https://github.com/clone2727/smushplay/blob/master/codec47.cpp
 *  https://github.com/clone2727/smushplay/blob/master/codec48.cpp
 *
 * Others were reversed from the various game executables.
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

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

#define be32_to_cpu(x)  (x)
#define be16_to_cpu(x)  (x)
#define le32_to_cpu(x)  bswap_32(x)
#define le16_to_cpu(x)  bswap_16(x)
#define cpu_to_le16(x)  bswap_16(x)

#else

#error "unknown endianness"

#endif

/* bytewise read an unaligned 16bit value from memory */
static inline uint16_t ua16(uint8_t *p)
{
	return p[0] | p[1] << 8;
}

/* bytewise read an unaligned 32bit value from memory */
static inline uint32_t ua32(uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

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
#define PVOC	0x434f5650
#define PSD2	0x32445350
#define SANM	0x4d4e4153
#define SHDR	0x52444853
#define FLHD	0x44484c46
#define BL16	0x36316c42
#define WAVE	0x65766157
#define ANNO	0x4f4e4e41
#define IMA4	0x494d4134

/* reasonable maximum size of a FRME */
#define FRME_MAX_SIZE	(4 << 20)

/* ANIM maximum image size */
#define FOBJ_MAXX	640
#define FOBJ_MAXY	480

/* sizes of various internal work buffers */
#define SZ_IACT		(4096)
#define SZ_PAL		(256 * sizeof(uint32_t))
#define SZ_DELTAPAL	(768 * sizeof(int16_t))
#define SZ_SHIFTPAL	(768 * sizeof(int16_t))
#define SZ_C47IPTBL	(256 * 256)
#define SZ_ADSTBUF	(393216)
#define SZ_ANMBUFS (SZ_IACT + SZ_PAL + SZ_DELTAPAL + SZ_C47IPTBL + SZ_SHIFTPAL)

/* codec47 glyhps */
#define GLYPH_COORD_VECT_SIZE 16
#define NGLYPHS 256

/* maximum volume. This is the maximum found in PSAD/iMUS streams */
#define ATRK_VOL_MAX	127

/* PSAD/iMUS multistream audio track */
#define ATRK_INUSE	(1 << 0)
#define ATRK_1CH	(1 << 1)
#define ATRK_SRC8BIT	(1 << 2)
#define ATRK_SRC12BIT	(1 << 3)
#define ATRK_MIXED	(1 << 4)
#define ATRK_BLOCKED	(1 << 5)
#define ATRK_REMIX	(1 << 6)

/* 1M for Dig */
#define _ATRK_DATSZ	(1 << 20)
_Static_assert((_ATRK_DATSZ > 0) && ((_ATRK_DATSZ & (_ATRK_DATSZ - 1)) == 0),
	       "_ATRK_DATSZ must be a power of 2.");

static const uint32_t ATRK_DATSZ = _ATRK_DATSZ;
static const uint32_t ATRK_DATMASK = (_ATRK_DATSZ - 1);

#define ATRK_MAX	10
#define ATRK_MAX_STRK_SIZE	3072

/* PSAD flags defining audio track type (voice/music/sfx).
 * ANIMv1 could only differentiate between them based on the chunk name,
 * while ANIMv2 added these flags to the PSAD header itself.
 */
#define SAUD_FLAG_TRK_MASK	0xc0
#define SAUD_FLAG_TRK_VOICE	0x80
#define SAUD_FLAG_TRK_MUSIC	0x40
#define SAUD_FLAG_TRK_SFX	0x00


/* function to read the source data and convert it to 16bit sample,
 * at offset i, for channel c
 */
struct sanatrk;
struct sanmsa;

typedef int16_t(*atrk_decode)(struct sanatrk*, uint32_t i, uint8_t c);

struct sanatrk {
	/* ATRK fields */
	uint8_t *data;		/* input data buffer			*/
	uint32_t rdptr;		/* read pointer				*/
	uint32_t wrptr;		/* write pointer			*/
	int32_t datacnt;	/* currently held data in buffer	*/
	uint32_t flags;		/* ATRK_ flags				*/
	atrk_decode decode;	/* upmix function			*/
	int32_t dataleft;	/* SAUD data left until track ends	*/
	uint32_t dstfavail;	/* frames in dest format available	*/
	uint32_t src_accum;	/* SRC accumulator 16.16		*/
	uint32_t src_cnvrate;	/* SRC conversion ratio 16.16		*/
	uint16_t srate;		/* source sample rate			*/

	/* PSAD/SAUD-related fields */
	uint16_t trkid;		/* ID of this track			*/
	uint16_t curridx;	/* index of the last PSAD chunk		*/
	uint16_t maxidx;	/* expected highest PSAD chunk index	*/
	uint16_t pflags;	/* PSAD flags/class (music/sfx/voice)	*/
	uint16_t vol;		/* PSAD/iMUS stream volume		*/
	int8_t pan;		/* PSAD stream pan			*/
	uint32_t playlen;	/* amount of bytes still to play	*/
	uint32_t dstpavail;	/* frames in dest format still to play	*/
	uint32_t af0;		/* 435af0 of the channel		*/
	uint32_t af4;		/* 435af4 of the channel		*/
	uint8_t strk[ATRK_MAX_STRK_SIZE];	/* STRK opcodes		*/
	uint16_t strkptr;	/* STRK pc				*/
	uint16_t strksz;	/* STRK data size			*/
	struct sanmsa *msa;	/* ptr back to shared parent		*/
};

/* Multistream Audio container */
struct sanmsa {
	struct sanatrk atrk[ATRK_MAX];	/* audio tracks 		*/
	uint8_t *audrsb1;	/* 8 generic 1-frame resample dest buf	*/
	uint32_t audminframes;	/* 4 dest sample rate frames for a frame*/
	uint8_t sou_hooks[256];		/* 256 PSAD shared sound hooks	*/
	uint16_t sou_vol_sfx;			/* sfx volume		*/
	uint16_t sou_vol_voice;			/* voice volume		*/
	uint16_t sou_vol_music;			/* global music volume	*/
	uint16_t sou_vol_global;		/* overall volume	*/
	int16_t sou_vol_damp;		/* damping volume, 255 no damp	*/
	uint16_t sou_damp_min;		/* minimum damping volume	*/
	uint16_t sou_damp_max;		/* maximum damping volume	*/
	uint16_t sou_damp_dip_rate;	/* damping volume reduct. rate  */
	uint16_t sou_damp_rise_rate;	/* damping volume augment. rate */
	uint32_t samplerate;		/* 4 audio samplerate in Hz	*/
	uint8_t numtrk;
};

/* internal context: per-file */
struct sanrt {
	uint32_t frmebufsz;	/* 4 size of buffer below		*/
	uint8_t *fcache;	/* 8 one cached FRME object		*/
	uint8_t *fbuf;		/* 8 current front buffer		*/
	uint8_t *buf0;		/* 8 c37/47/48 front buffer		*/
	uint8_t *buf1;		/* 8 c47 delta buffer 1			*/
	uint8_t *buf2;		/* 8 c47 delta buffer 2			*/
	uint8_t *buf3;		/* 8 STOR buffer			*/
	uint8_t *buf4;		/* 8 last full frame for interpolation  */
	uint8_t *buf5;		/* 8 interpolated frame                 */
	uint8_t *vbuf;		/* 8 final image buffer passed to caller*/
	uint16_t pitch;		/* 2 image pitch			*/
	uint16_t bufw;		/* 2 alloc'ed buffer width/pitch	*/
	uint16_t bufh;		/* 2 alloc'ed buffer height		*/
	int16_t  lastseq;	/* 2 c47 last sequence id		*/
	uint16_t subid;		/* 2 subtitle message number		*/
	uint16_t to_store;	/* 2 STOR encountered			*/
	uint16_t currframe;	/* 2 current frame index		*/
	uint16_t iactpos;	/* 2 IACT buffer write pointer		*/
	uint8_t *iactbuf;	/* 8 4kB for IACT chunks 		*/
	uint8_t *c47ipoltbl;	/* 8 c47 interpolation table Compression 1 */
	int16_t  *deltapal;	/* 8 768x 16bit for XPAL chunks		*/
	int16_t *shiftpal;	/* 8 256x shifted pal			*/
	uint32_t *palette;	/* 8 256x ABGR				*/
	uint32_t fbsize;	/* 4 size of the framebuffers		*/
	uint32_t framedur;	/* 4 standard frame duration		*/
	uint16_t FRMEcnt;	/* 2 number of FRMEs in SAN		*/
	uint16_t version;	/* 2 SAN version number			*/
	uint8_t  have_vdims:1;	/* 1 we have valid video dimensions	*/
	uint8_t  have_frame:1;	/* 1 we have a valid video frame	*/
	uint8_t  have_itable:1;	/* 1 have c47/48 interpolation table    */
	uint8_t  can_ipol:1;	/* 1 do an interpolation                */
	uint8_t  have_ipframe:1;/* 1 we have an interpolated frame      */
	void	 *membase;	/* 8 base to allocated mem block	*/
};

/* internal context: static stuff. */
struct sanctx {
	struct sanrt rt;
	struct sanio *io;
	struct sanmsa *msa;	/* 8 ATRK infra				*/
	int errdone;		/* latest error status */
	uint8_t *adstbuf1;	/* 8 audio buffer 1			*/
		
	/* codec47 static data */
	int8_t c47_glyph4x4[NGLYPHS][16];
	int8_t c47_glyph8x8[NGLYPHS][64];
	uint8_t c4tbl[2][256][16];
	uint8_t c23lut[256];
	uint8_t c45tbl1[768];
	uint8_t c45tbl2[0x8000];
	uint16_t c4tblparam;
	uint16_t vima_pred_tbl[5786];
};

/* VIMA data tables */
static const uint8_t vima_size_table[] = {
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
};

static const int8_t vima_itbl1[] = {
	-1, 4, -1, 4
};

static const int8_t vima_itbl2[] = {
	-1, -1, 2, 6, -1, -1, 2, 6
};

static const int8_t vima_itbl3[] = {
	-1, -1, -1, -1, 1, 2, 4, 6, -1, -1, -1, -1, 1, 2, 4, 6
};

static const int8_t vima_itbl4[] = {
	-1, -1, -1, -1, -1, -1, -1, -1, 1,  1,  1,  2,  2,  4,  5,  6,
	-1, -1, -1, -1, -1, -1, -1, -1, 1,  1,  1,  2,  2,  4,  5,  6
};

static const int8_t vima_itbl5[] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	1,  1,  1,  1,  1,  2,  2,  2,  2,  4,  4,  4,  5,  5,  6,  6,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	1,  1,  1,  1,  1,  2,  2,  2,  2,  4,  4,  4,  5,  5,  6,  6
};

static const int8_t vima_itbl6[] = {
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,
	2,  2,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,
	2,  2,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,  6
};

static const int8_t *const vima_itbls[] = {
	vima_itbl1,  vima_itbl2,  vima_itbl3, vima_itbl4, vima_itbl5, vima_itbl6
};

/* adpcm standard step table */
#define ADPCM_STEP_COUNT 89
const int16_t adpcm_step_table[ADPCM_STEP_COUNT] = {
	7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
	19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
	50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
	130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
	337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
	876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
	5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t ima4_tab[16] = {
	-1, -1, -1, -1,  2,  4,  6,  8, -1, -1, -1, -1,  2,  4,  6,  8
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

static void c4_5_tilegen(uint8_t *dst, uint8_t param1)
{
	int i, j, k, l, m, n, o;

	// 23316
	for (i = 1; i < 16; i += 2) {			// i = l24
		// 23321
		for (k = 0; k < 16; k++) {		// k = bx
			// 23329
			j = i + param1;			// j = l34
			l = k + param1;			// k = l30
			m = (j + l) / 2;		// esi, dx
			n = (j + m) / 2;		// n = l28, l20 (16bit)
			o = (l + m) / 2;		// o = l28, l1c (16bit)
			if (j == m || l == m) {
				// 23376
				*dst++ = l; *dst++ = j; *dst++ = l; *dst++ = j;
				*dst++ = j; *dst++ = l; *dst++ = j; *dst++ = j;
				*dst++ = l; *dst++ = j; *dst++ = l; *dst++ = j;
				*dst++ = l; *dst++ = l; *dst++ = j; *dst++ = l;
			} else {
				// 233ab
				*dst++ = m; *dst++ = m; *dst++ = n; *dst++ = j;
				*dst++ = m; *dst++ = m; *dst++ = n; *dst++ = j;
				*dst++ = o; *dst++ = o; *dst++ = m; *dst++ = n;
				*dst++ = l; *dst++ = l; *dst++ = o; *dst++ = m;
			}
		}
	}

	// 23415
	for (i = 0; i < 16; i += 2) {			// i = l24
		// 23420
		for (k = 0; k < 16; k++) {		// k = bx
			// 23428
			j = i + param1;			// j = l34
			l = k + param1;			// l = l2c
			m = (j + l) / 2;		// m = si, dx
			n = (j + m) / 2;		// n = l28, l20 (16bit)
			o = (l + m) / 2;		// o = l28, l1c (16bit)
			if (m == j || m == l) {
				// 23477
				*dst++ = j; *dst++ = j; *dst++ = l; *dst++ = j;
				*dst++ = j; *dst++ = j; *dst++ = j; *dst++ = l;
				*dst++ = l; *dst++ = j; *dst++ = l; *dst++ = l;
				*dst++ = j; *dst++ = l; *dst++ = j; *dst++ = l;
			} else {
				// 234b1  l14 = j
				*dst++ = j; *dst++ = j; *dst++ = n; *dst++ = m;
				*dst++ = j; *dst++ = j; *dst++ = n; *dst++ = m;
				*dst++ = n; *dst++ = n; *dst++ = m; *dst++ = o;
				*dst++ = m; *dst++ = m; *dst++ = o; *dst++ = l;
			}
		}
	}
}

static void c33_34_tilegen(uint8_t *dst, int8_t param1)
{
	int i, j, k, l, m, n, o, p;

	/* ASSAULT.EXE 1bf7b */
	for (i = 0; i < 8; i++) {
		for (k = 0; k < 8; k++) {
			j = i + param1;
			l = k + param1;
			p = (j + l) >> 1;
			n = (j + p) >> 1;
			m = (p + l) >> 1;

			*dst++ = p; *dst++ = p; *dst++ = n; *dst++ = j;
			*dst++ = p; *dst++ = p; *dst++ = n; *dst++ = j;
			*dst++ = m; *dst++ = m; *dst++ = p; *dst++ = j;
			*dst++ = l; *dst++ = l; *dst++ = m; *dst++ = p;
		}
	}

	for (i = 0; i < 8; i++) {
		for (k = 0; k < 8; k++) {
			j = i + param1;
			l = k + param1;
			n = (j + l) >> 1;
			m = (l + n) >> 1;

			*dst++ = j; *dst++ = j; *dst++ = j; *dst++ = j;
			*dst++ = n; *dst++ = n; *dst++ = n; *dst++ = n;
			*dst++ = m; *dst++ = m; *dst++ = m; *dst++ = m;
			*dst++ = l; *dst++ = l; *dst++ = l; *dst++ = l;
		}
	}

	for (i = 0; i < 8; i++)	{
		for (k = 0; k < 8; k++) {
			j = i + param1;
			l = k + param1;
			m = (j + l) >> 1;
			n = (j + m) >> 1;
			o = (l + m) >> 1;

			*dst++ = j; *dst++ = j; *dst++ = n; *dst++ = m;
			*dst++ = j; *dst++ = j; *dst++ = n; *dst++ = m;
			*dst++ = n; *dst++ = n; *dst++ = m; *dst++ = o;
			*dst++ = m; *dst++ = m; *dst++ = o; *dst++ = l;
		}
	}

	for (i = 0; i < 8; i++) {
		for (k = 0; k < 8; k++) {
			j = i + param1;
			l = k + param1;
			m = (j + l) >> 1;
			n = (l + m) >> 1;

			*dst++ = j; *dst++ = m; *dst++ = n; *dst++ = l;
			*dst++ = j; *dst++ = m; *dst++ = n; *dst++ = l;
			*dst++ = j; *dst++ = m; *dst++ = n; *dst++ = l;
			*dst++ = j; *dst++ = m; *dst++ = n; *dst++ = l;
		}
	}
}

static void c4_5_param2(struct sanctx *ctx, uint8_t *src, uint16_t cnt,
		       uint8_t clr)
{
	uint8_t c, *dst;
	uint32_t loop;

	/* ASSALT13.EXE 11bd0 - 11c16 */
	loop = cnt << 2;
	dst = (uint8_t *)&(ctx->c4tbl[1]);
	while (loop--) {
		c = *src++;			// lodsb [*esi into al] | xor ah,ah vor schleife
		*dst++ = (c >> 4) + clr;	// ror ax, 4; add al, bl; stosb
		*dst++ = (c & 0xf) + clr;	// rol ax, 4; and ax, 0xf, add al, bl; stosb
		c = *src++;
		*dst++ = (c >> 4) + clr;
		*dst++ = (c & 0xf) + clr;
	}
}

/******************************************************************************/

static void blt_solid(uint8_t *dst, uint8_t *src, int16_t left, int16_t top,
		      uint16_t srcxoff, uint16_t srcyoff, uint16_t srcwidth,
		      uint16_t srcheight, uint16_t srcpitch, uint16_t dstpitch,
		      uint16_t dstheight, int32_t size)
{
	if ((srcwidth == 0) || (srcheight == 0) || (size < 1))
		return;
	if (top < 0) {
		if (-top >= srcheight)
			return;
		srcyoff -= top;
		srcheight += top;
		top = 0;
		size -= (srcpitch * -top);
	}

	if ((top + srcheight) > dstheight) {
		int clip = (top + srcheight) - dstheight;
		if (clip >= srcheight)
			return;
		srcheight -= clip;
	}

	if (left < 0) {
		if (-left >= srcwidth)
			return;
		srcxoff -= left;
		srcwidth += left;
		size += left;
		left = 0;
	}

	if (left + srcwidth > dstpitch) {
		int clip = (left + srcwidth) - dstpitch;
		if (clip >= srcwidth)
			return;
		srcwidth -= clip;
	}
	src += ((uintptr_t)srcyoff * srcpitch) + srcxoff;
	dst += ((uintptr_t)top * dstpitch) + left;
	while ((srcheight--) && (size >= srcwidth)) {
		memcpy(dst, src, srcwidth);
		src += srcpitch;
		dst += dstpitch;
		size -= srcpitch;
	}
	if ((size > 0) && (size < srcwidth) && (srcheight > 0))
		memcpy(dst, src, size);
}

static void blt_mask(uint8_t *dst, uint8_t *src, int16_t left, int16_t top,
		     uint16_t srcxoff, uint16_t srcyoff, uint16_t srcwidth,
		     uint16_t srcheight, uint16_t srcpitch, uint16_t dstpitch,
		     uint16_t dstheight, int32_t size, uint8_t skipcolor)
{
	if ((srcwidth == 0) || (srcheight == 0) || (size < 1))
		return;
	if (top < 0) {
		if (-top >= srcheight)
			return;
		srcyoff -= top;
		srcheight += top;
		top = 0;
		size -= (srcpitch * -top);
	}

	if ((top + srcheight) > dstheight) {
		int clip = (top + srcheight) - dstheight;
		if (clip >= srcheight)
			return;
		srcheight -= clip;
	}

	if (left < 0) {
		if (-left >= srcwidth)
			return;
		srcxoff -= left;
		srcwidth += left;
		size += left;
		left = 0;
	}

	if (left + srcwidth > dstpitch) {
		int clip = (left + srcwidth) - dstpitch;
		if (clip >= srcwidth)
			return;
		srcwidth -= clip;
	}
	src += ((uintptr_t)srcyoff * srcpitch) + srcxoff;
	dst += ((uintptr_t)top * dstpitch) + left;
	for (int i = 0; (size > 0) && (i < srcheight); i++) {
		for (int j = 0; (size > 0) && (j < srcwidth); j++, size--) {
			if (src[j] != skipcolor)
				dst[j] = src[j];
		}
		src += srcpitch;
		dst += dstpitch;
	}
}

static void blt_ipol(uint8_t *dst, uint8_t *src1, uint8_t *src2, int16_t left,
		     int16_t top, uint16_t srcxoff, uint16_t srcyoff,
		     uint16_t srcwidth, uint16_t srcheight, uint16_t srcpitch,
		     uint16_t dstpitch, uint16_t dstheight, int32_t size,
		     uint8_t *itbl)
{
	if ((srcwidth == 0) || (srcheight == 0) || (size < 1))
		return;
	if (top < 0) {
		if (-top >= srcheight)
			return;
		srcyoff -= top;
		srcheight += top;
		top = 0;
		size -= (srcpitch * -top);
	}

	if ((top + srcheight) > dstheight) {
		int clip = (top + srcheight) - dstheight;
		if (clip >= srcheight)
			return;
		srcheight -= clip;
	}

	if (left < 0) {
		if (-left >= srcwidth)
			return;
		srcxoff -= left;
		srcwidth += left;
		size += left;
		left = 0;
	}

	if (left + srcwidth > dstpitch) {
		int clip = (left + srcwidth) - dstpitch;
		if (clip >= srcwidth)
			return;
		srcwidth -= clip;
	}
	src1 += ((uintptr_t)srcyoff * srcpitch) + srcxoff;
	src2 += ((uintptr_t)srcyoff * srcpitch) + srcxoff;
	dst += ((uintptr_t)top * dstpitch) + left;
	for (int i = 0; (size > 0) && (i < srcheight); i++) {
		for (int j = 0; (size > 0) && (j < srcwidth); j++, size--) {
			dst[j] = itbl[(src1[j] << 8) | src2[j]];
		}
		src1 += srcpitch;
		src2 += srcpitch;
		dst += dstpitch;
	}
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
		*pal++ = 0xffU << 24 | t[2] << 16 | t[1] << 8 | t[0];
		i++;
	}
	/* HACK: (not sure though if really a hack): palette index 0
	 * is always 0/0/0 for RA1. Fixes a lot of scenes with blue/
	 * white/gray/.. space backgrounds, at least until the next NPAL.
	 */
	if (rt->version < 2)
		rt->palette[0] = 0xffU << 24;
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
			      uint8_t *p1, uint8_t *p2, const uint16_t w,
			      const uint8_t *coltbl, uint16_t size, uint32_t *dsize)
{
	uint8_t opc, col[2], c;
	uint16_t i, j;
	int8_t *pglyph;

	if ((*dsize) < 1)
		return 0;
	opc = *src++;
	(*dsize)--;
	if (opc >= 0xF8) {
		switch (opc) {
		case 0xff:
			if (size == 2) {
				if ((*dsize) < 4)
					return 0;
				*(dst + 0 + 0) = *src++; *(dst + 0 + 1) = *src++;
				*(dst + w + 0) = *src++; *(dst + w + 1) = *src++;
				(*dsize) -= 4;
			} else {
				size >>= 1;
				src = codec47_block(ctx, src, dst, p1, p2, w, coltbl, size, dsize);
				if (!src)
					return 0;
				src = codec47_block(ctx, src, dst + size, p1 + size, p2 + size, w, coltbl, size, dsize);
				if (!src)
					return 0;
				dst += (size * w);
				p1 += (size * w);
				p2 += (size * w);
				src = codec47_block(ctx, src, dst, p1, p2, w, coltbl, size, dsize);
				if (!src)
					return 0;
				src = codec47_block(ctx, src, dst + size, p1 + size, p2 + size, w, coltbl, size, dsize);
			}
			break;
		case 0xfe:
			if ((*dsize) < 1)
				return 0;
			c = *src++;
			(*dsize)--;
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = c;
			break;
		case 0xfd:
			if ((*dsize) < 3)
				return 0;
			opc = *src++;
			col[1] = *src++;
			col[0] = *src++;
			pglyph = (size == 8) ? ctx->c47_glyph8x8[opc] : ctx->c47_glyph4x4[opc];
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = col[*pglyph++];
			(*dsize) -= 3;
			break;
		case 0xfc:
			for (i = 0; i < size; i++)
				for (j = 0; j < size; j++)
					*(dst + (i * w) + j) = *(p1 + (i * w) + j);
			break;
		default:	/* fill a block with color from the codebook */
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

static int codec47_comp2(struct sanctx *ctx, uint8_t *src, uint8_t *dst,
			 const uint16_t w, const uint16_t h, const uint8_t *coltbl, uint32_t size)
{
	uint8_t *b1 = ctx->rt.buf1, *b2 = ctx->rt.buf2;
	unsigned int i, j;

	for (j = 0; (j < h) && src && size; j += 8) {
		for (i = 0; (i < w) && src && size; i += 8) {
			src = codec47_block(ctx, src, dst + i, b1 + i, b2 + i, w, coltbl, 8, &size);
		}
		dst += (w * 8);
		b1 += (w * 8);
		b2 += (w * 8);
	}
	return (src == 0) ? 1 : 0;
}

static void codec47_comp5(uint8_t *src, uint32_t size, uint8_t *dst, uint32_t left)
{
	uint8_t opc, rlen, col, j;

	while (left && size) {
		opc = *src++;
		size--;
		rlen = (opc >> 1) + 1;
		if (rlen > left)
			rlen = left;
		if (opc & 1) {
			if (size < 1)
				return;
			col = *src++;
			size--;
			for (j = 0; j < rlen; j++)
				*dst++ = col;
		} else {
			if (size < rlen)
				return;
			for (j = 0; j < rlen; j++)
				*dst++ = *src++;
			size -= rlen;
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

static int codec47(struct sanctx *ctx, uint8_t *dbuf, uint8_t *src, uint16_t w, uint16_t h,
		   int16_t top, int16_t left, uint32_t size)
{
	uint8_t *coltbl, comp, newrot, flag, *dst;
	uint32_t decsize;
	uint16_t seq;

	if (size < 26)
		return -60;

	seq =    le16_to_cpu(*(uint16_t *)(src + 0));
	comp =   src[2];
	newrot = src[3];
	flag =   src[4];
	coltbl = src + 8;	/* codebook 8 colors */
	decsize  = le32_to_cpu(ua32(src + 14));	/* decoded (raw frame) size */
	if (decsize > ctx->rt.fbsize)
		decsize = ctx->rt.fbsize;

	if (seq == 0) {
		ctx->rt.lastseq = -1;
		memset(ctx->rt.buf1, src[12], decsize);
		memset(ctx->rt.buf2, src[13], decsize);
	}
	src += 26;
	size -= 26;
	if (flag & 1) {
		if (size < 0x8080)
			return -61;
		codec47_itable(ctx, src);
		src += 0x8080;
		size -= 0x8080;
	}

	dst = ctx->rt.buf0;
	switch (comp) {
	case 0:	if (size < w * h)
			return -62;
		memcpy(dst, src, w * h);
		break;
	case 1:	if (size < ((w * h) / 4))
			return -63;
		codec47_comp1(src, dst, ctx->rt.c47ipoltbl, w, h);
		break;
	case 2:	if (seq == (ctx->rt.lastseq + 1)) {
			if (0 != codec47_comp2(ctx, src, dst, w, h, coltbl, size))
				return -64;
		}
		break;
	case 3:	memcpy(dst, ctx->rt.buf2, ctx->rt.fbsize); break;
	case 4:	memcpy(dst, ctx->rt.buf1, ctx->rt.fbsize); break;
	case 5:	codec47_comp5(src, size, dst, decsize); break;
	default: break;
	}

	blt_solid(dbuf, dst, left, top, 0, 0, w, h, w, ctx->rt.pitch, ctx->rt.bufh, w * h);

	if (seq == ctx->rt.lastseq + 1)
		c47_swap_bufs(ctx, newrot);

	ctx->rt.lastseq = seq;
	if (seq > 1)
		ctx->rt.can_ipol = 1;

	return 0;
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
static uint8_t *c48_block(uint8_t *src, uint8_t *dst, uint8_t *db, const uint16_t w, const uint8_t *itbl, uint32_t *size)
{
	uint8_t opc, sb[16];
	int16_t mvofs;
	uint32_t ofs;
	int i, j, k, l;

	if (*size < 1)
		return 0;
	opc = *src++;
	(*size)--;
	switch (opc) {
	case 0xFF:	/* 1x1 -> 8x8 block scale */
		if (*size < 1)
			return 0;
		(*size)--;
		sb[15] = *src++;
		sb[ 7] = itbl[(*(dst - 1*w + 7) << 8) | sb[15]];
		sb[ 3] = itbl[(*(dst - 1*w + 7) << 8) | sb[ 7]];
		sb[11] = itbl[(sb[15] << 8)           | sb[ 7]];
		sb[ 1] = itbl[(*(dst - 0*w - 1) << 8) | sb[ 3]];
		sb[ 0] = itbl[(*(dst - 0*w - 1) << 8) | sb[ 1]];
		sb[ 2] = itbl[(sb[ 3] << 8)           | sb[ 1]];
		sb[ 5] = itbl[(*(dst + 2*w -1) << 8)  | sb[ 7]];
		sb[ 4] = itbl[(*(dst + 2*w -1) << 8)  | sb[ 5]];
		sb[ 6] = itbl[(sb[ 7] << 8)           | sb[ 5]];
		sb[ 9] = itbl[(*(dst + 3*w -1) << 8)  | sb[11]];
		sb[ 8] = itbl[(*(dst + 3*w -1) << 8)  | sb[ 9]];
		sb[10] = itbl[(sb[11] << 8)           | sb[ 9]];
		sb[13] = itbl[(*(dst + 4*w -1) << 8)  | sb[15]];
		sb[12] = itbl[(*(dst + 4*w -1) << 8)  | sb[13]];
		sb[14] = itbl[(sb[15] << 8)           | sb[13]];
		c48_4to8(dst, sb, w);
		break;
	case 0xFE:	/* 1x 8x8 copy from deltabuf, 16bit mv from src */
		if ((*size) < 2)
			return 0;
		mvofs = (int16_t)le16_to_cpu(ua16(src)); src += 2;
		for (i = 0; i < 8; i++) {
			ofs = w * i;
			for (k = 0; k < 8; k++)
				*(dst + ofs + k) = *(db + ofs + k + mvofs);
		}
		(*size) -= 2;
		break;
	case 0xFD:	/* 2x2 -> 8x8 block scale */
		if ((*size) < 4)
			return 0;
		sb[ 5] = *src++;
		sb[ 7] = *src++;
		sb[13] = *src++;
		sb[15] = *src++;
		sb[ 1] = itbl[(*(dst - 1*w + 3) << 8) | sb[ 5]];
		sb[ 3] = itbl[(*(dst - 1*w + 7) << 8) | sb[ 7]];
		sb[11] = itbl[(sb[15] << 8)           | sb[ 7]];
		sb[ 9] = itbl[(sb[13] << 8)           | sb[ 5]];
		sb[ 0] = itbl[(*(dst - 0*w - 1) << 8) | sb[ 1]];
		sb[ 2] = itbl[(sb[ 3] << 8)           | sb[ 1]];
		sb[ 4] = itbl[(*(dst + 2*w - 1) << 8) | sb[ 5]];
		sb[ 6] = itbl[(sb[ 7] << 8)           | sb[ 5]];
		sb[ 8] = itbl[(*(dst + 3*w - 1) << 8) | sb[ 9]];
		sb[10] = itbl[(sb[11] << 8)           | sb[ 9]];
		sb[12] = itbl[(*(dst + 4*w - 1) << 8) | sb[13]];
		sb[14] = itbl[(sb[15] << 8)           | sb[13]];
		c48_4to8(dst, sb, w);
		(*size) -= 4;
		break;
	case 0xFC:	/* 4x copy 4x4 block, per-block c48_mv, index from source */
		if ((*size) < 4)
			return 0;
		for (i = 0; i < 8; i += 4) {
			for (k = 0; k < 8; k += 4) {
				opc = *src++;
				opc = (opc == 255) ? 0 : opc;
				mvofs = c37_mv[0][opc * 2] + (c37_mv[0][opc * 2 + 1] * w);
				for (j = 0; j < 4; j++) {
					ofs = (w * (j + i)) + k;
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *(db + ofs + l + mvofs);
				}
			}
		}
		(*size) -= 4;
		break;
	case 0xFB: 	/* Copy 4x 4x4 blocks, per-block mv from source */
		if ((*size) < 8)
			return 0;
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
		(*size) -= 8;
		break;
	case 0xFA:	/* scale 4x4 input block to 8x8 dest block */
		if ((*size) < 16)
			return 0;
		c48_4to8(dst, src, w);
		src += 16;
		(*size) -= 16;
		break;
	case 0xF9:	/* 16x 2x2 copy from delta, per-block c48_mv */
		if ((*size) < 16)
			return 0;
		for (i = 0; i < 8; i += 2) {				/* 4 */
			for (j = 0; j < 8; j += 2) {			/* 4 */
				ofs = (w * i) + j;
				opc = *src++;
				opc = (opc == 255) ? 0 : opc;
				mvofs = c37_mv[0][opc * 2] + (c37_mv[0][opc * 2 + 1] * w);
				for (l = 0; l < 2; l++) {
					*(dst + ofs + l + 0) = *(db + ofs + l + 0 + mvofs);
					*(dst + ofs + l + w) = *(db + ofs + l + w + mvofs);
				}
			}
		}
		(*size) -= 16;
		break;
	case 0xF8:	/* 16x 2x2 blocks copy, mv from source */
		if ((*size) < 32)
			return 0;
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
		(*size) -= 32;
		break;
	case 0xF7:	/* copy 8x8 block from src to dest */
		if ((*size) < 64)
			return 0;
		for (i = 0; i < 8; i++) {
			ofs = i * w;
			for (l = 0; l < 8; l++)
				*(dst + ofs + l) = *src++;
		}
		(*size) -= 64;
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

static int codec48_comp3(uint8_t *src, uint8_t *dst, uint8_t *db,
			 uint8_t *itbl, uint16_t w, uint16_t h, uint32_t size)
{
	int i, j;

	for (i = 0; (i < h) && src && size; i += 8) {
		for (j = 0; (j < w) && src && size; j += 8) {
			src = c48_block(src, dst + j, db + j, w, itbl, &size);
		}
		dst += w * 8;
		db += w * 8;
	}
	return (src == 0) ? 1 : 0;
}

static int codec48(struct sanctx *ctx, uint8_t *dbuf, uint8_t *src, uint16_t w,
		   uint16_t h, int16_t top, int16_t left, uint32_t size)
{
	uint32_t pktsize, decsize;
	uint8_t comp, flag, *dst;
	uint16_t seq;

	if (size < 16)
		return -80;

	comp =	src[0];		/* subcodec */
	if (src[1] != 1)	/* mvec table variant, always 1 with MotS */
		return -22;

	seq = le16_to_cpu(*(uint16_t*)(src + 2));

	/* decsize is the size of the raw frame aligned to 8x8 blocks
	 * when seq == 0, otherwise it's identical to pktsize, which
	 * indicates the number of bytes in the datastream for this packet.
	 */
	decsize = le32_to_cpu(ua32(src + 4));
	pktsize = le32_to_cpu(ua32(src + 8));
	flag =	src[12];
	/* known flag values:
	 * 0x01: skip decoding of comp3 if sequence number is odd and 0x10 is not set.
	 *       used in MM only.
	 * 0x02: if set: copy result line by line, skip a custom color (0).
	 *       this is only used in "Making Magic"; in MotS setting this bit does not
	 *       blit the result to the front buffer (i.e. previous frame is kept), but
	 *       not used in any MotS videos.
	 * 0x04: unknown, always set, never checked for in MM or MotS.
	 * 0x08: interpolation table data (0x8080 bytes) follows after the header.
	 * 0x10: interpolate a frame using the 2 buffers and interpolation table, i.e.
	 *        do not blit the current main buffer to destination, but rather create
	 *        an intermediate frame using the interpolation table. Used in MM.
	 *       The next frame then is compression 6, i.e. blits the actual decoding
	 *        result to main buffer.
	 * 0x20: unknown, checked in error path in MM
	 * 0x30: unknonw, same as 0x20 but checked for MotS error path.
	 */

	if (decsize > ctx->rt.fbsize)
		decsize = ctx->rt.fbsize;
	if (pktsize > ctx->rt.fbsize)
		pktsize = ctx->rt.fbsize;

	if (seq == 0) {
		/* keep buf0 for comp == 6 */
		memset(ctx->rt.buf2, 0, decsize);
	}

	src += 16;
	size -= 16;
	if (flag & 8) {
		if (size < 0x8080)
			return -81;
		codec47_itable(ctx, src);
		src += 0x8080;
		size -= 0x8080;
	}

	dst = ctx->rt.buf0;
	switch (comp) {
	case 0:	if (size < pktsize)
			return -82;
		memcpy(dst, src, pktsize);
		break;
	case 2: codec47_comp5(src, size, dst, decsize); break;
	case 3: if ((seq == 0) || (seq == ctx->rt.lastseq + 1)) {
			if ((seq & 1) || ((flag & 1) == 0) || (flag & 0x10)) {
				c47_swap_bufs(ctx, 1);	/* swap 0 and 2 */
				dst = ctx->rt.buf0;
			}
			if (0 != codec48_comp3(src, dst, ctx->rt.buf2, ctx->rt.c47ipoltbl, w, h, size))
				return -84;
		}
		break;
	case 5: if (size < ((w * h) / 4))
			return -83;
		codec47_comp1(src, dst, ctx->rt.c47ipoltbl, w, h);
		break;
	default:
		break;
	}

	if (seq > 0)
		ctx->rt.can_ipol = 1;
	ctx->rt.lastseq = seq;

	if ((flag & 2) == 0) {
		if (flag & 0x10) {
			blt_ipol(dbuf, ctx->rt.buf0, ctx->rt.buf2, left, top,
				 0, 0, w, h, w, ctx->rt.pitch, ctx->rt.bufh, w * h,
				 ctx->rt.c47ipoltbl);
			ctx->rt.can_ipol = 0;
			return 0;
		}
		blt_solid(dbuf, dst, left, top, 0, 0, w, h, w, ctx->rt.pitch,
			  ctx->rt.bufh, w * h);
	} else {
		blt_mask(dbuf, dst, left, top, 0, 0, w, h, w, ctx->rt.pitch,
			 ctx->rt.bufh, w * h, 0);
	}

	return 0;
}

/******************************************************************************/

static void codec37_comp1(uint8_t *src, uint32_t size, uint8_t *dst, uint8_t *db,
			  uint16_t w, uint16_t h, uint8_t mvidx)
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
				if (size < 1)
					return;
				len = (*src) >> 1;
				run = !!((*src++) & 1);
				skip = 0;
				size--;
			} else {
				skip = run;
			}

			if (!skip) {
				if (size < 1)
					return;
				opc = *src++;
				size--;
				if (opc == 0xff) {
					len--;
					for (k = 0; k < 4; k++) {
						ofs = j + (k * w);
						for (l = 0; l < 4; l++) {
							if (len < 0) {
								if (size < 1)
									return;
								len = (*src) >> 1;
								run = !!((*src++) & 1);
								size--;
								if (run && size) {
									opc = *src++;
									size--;
								}
							}
							if (!run){
								if (size < 1) {
									return;
								} else {
									*(dst + ofs + l) = *src++;
									size--;
								}
							} else {
								*(dst + ofs + l) = opc;
							}
							len--;
						}
					}
					continue;
				}
			}
			/* 4x4 block copy from prev with MV */
			opc = (opc == 255) ? 0 : opc;
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
			  uint8_t mvidx, const uint8_t f4, const uint8_t c4, uint32_t size)
{
	uint8_t opc, c, copycnt;
	int32_t ofs, mvofs;
	int i, j, k, l;

	copycnt = 0;
	for (i = 0; i < h; i += 4) {
		for (j = 0; j < w; j += 4) {

			/* copy a 4x4 block from the previous frame from same spot */
			if (copycnt > 0) {
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++) {
						*(dst + ofs + l) = *(db + ofs + l);
					}
				}
				copycnt--;
				continue;
			}

			if (size < 1)
				return;
			opc = *src++;
			size--;
			if (opc == 0xff) {
				/* 1 4x4 block, per-pixel data from source */
				if (size < 16)
					return;
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *src++;
				}
				size -= 16;
			} else if (f4 && (opc == 0xfe)) {
				/* 4 2x2 blocks, per-block color from source */
				if (size < 4)
					return;
				for (k = 0; k < 4; k += 2) {
					for (l = 0; l < 2; l++) {
						*(dst + j + ((k + l) * w) + 0) = src[0];
						*(dst + j + ((k + l) * w) + 1) = src[0];
						*(dst + j + ((k + l) * w) + 2) = src[1];
						*(dst + j + ((k + l) * w) + 3) = src[1];
					}
					src += 2;
				}
				size -= 4;
			} else if (f4 && (opc == 0xfd)) {
				/* 1 4x4 block, block color from source */
				if (size < 1)
					return;
				c = *src++;
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = c;
				}
				size--;
			} else {
				/* 4x4 block copy from prev with MV */
				mvofs = c37_mv[mvidx][opc*2] + (c37_mv[mvidx][opc*2 + 1] * w);
				for (k = 0; k < 4; k++) {
					ofs = j + (k * w);
					for (l = 0; l < 4; l++)
						*(dst + ofs + l) = *(db + ofs + l + mvofs);
				}
				/* comp 4 opcode 0 indicates run start */
				if (c4 && (opc == 0)) {
					if (size < 1)
						return;
					copycnt = *src++;
					size--;
				}
			}
		}
		dst += w * 4;
		db += w * 4;
	}
}

static int codec37(struct sanctx *ctx, uint8_t *dbuf, uint8_t *src, uint16_t w,
		   uint16_t h, int16_t top, int16_t left, uint32_t size)
{
	uint8_t comp, mvidx, flag, *dst, *db;
	uint32_t decsize;
	uint16_t seq;

	if (size < 16)
		return -70;

	comp = src[0];
	mvidx = src[1];
	if (mvidx > 2)
		return -21;
	seq = le16_to_cpu(*(uint16_t *)(src + 2));
	decsize = le32_to_cpu(ua32(src + 4));
	flag = src[12];

	if (decsize > ctx->rt.fbsize)
		decsize = ctx->rt.fbsize;

	if (comp == 0 || comp == 2)
		memset(ctx->rt.buf2, 0, decsize);

	if ((comp == 1 || comp == 3 || comp == 4)
	    && ((seq & 1) || !(flag & 1))) {
		void *tmp = ctx->rt.buf0;
		ctx->rt.buf0 = ctx->rt.buf2;
		ctx->rt.buf2 = tmp;
	}

	src += 16;
	size -= 16;
	dst = ctx->rt.buf0;
	db = ctx->rt.buf2;

	switch (comp) {
	case 0: memcpy(dst, src, _min(size, decsize)); break;
	case 1: codec37_comp1(src, size, dst, db, w, h, mvidx); break;
	case 2: codec47_comp5(src, size, dst, decsize); break;
	case 3: /* fallthrough */
	case 4: codec37_comp3(src, dst, db, w, h, mvidx, flag & 4, comp == 4, size); break;
	default: break;
	}

	ctx->rt.lastseq = seq;

	if ((flag & 2) == 0) {
		blt_solid(dbuf, dst, left, top, 0, 0, w, h, w, ctx->rt.pitch,
			  ctx->rt.bufh, w * h);
	} else {
		blt_mask(dbuf, dst, left, top, 0, 0, w, h, w, ctx->rt.pitch,
			 ctx->rt.bufh, w * h, 0);
	}

	return 0;
}

/******************************************************************************/

static void codec45(struct sanctx *ctx, uint8_t *dst_in, uint8_t *src, uint16_t w, uint16_t h,
		    int16_t top, int16_t left, uint16_t size, uint8_t param,
		    uint16_t param2)
{
	uint8_t *tbl1 = ctx->c45tbl1, *tbl2 = ctx->c45tbl2, *dst;
	const uint16_t pitch = ctx->rt.pitch;
	unsigned int t1, c1, c2, w1, w2, w3;
	int32_t xoff, yoff;
	int i, b1, b2, xd;

	/* RA2 32b00 */
	if ((size < 6) || (src[4] != 1))
		return;

	t1 = *(uint16_t *)(src + 2);
	if (t1 == 0) {
		if (size < 0x306)
			return;
		memcpy(tbl1, src + 6, 0x300);
		src += 0x306;
		size -= 0x306;
		/* if you follow the assembly (32b49 - 32b6d), EAX is 1 when the
		 * first store to tbl2 is done.  However this is of course wrong and
		 * results in wrong colors.
		 */
		i = 0;
		while ((size > 1) && (i < 0x8000)) {
			b2 = *src++;
			if ((b2 + i) > 0x8000)
				b2 = 0x8000 - i;
			memset(tbl2 + i, *src++, b2);
			i += b2;
			size -= 2;
		}
	} else {
		src += 6;
		size -= 6;
	}
	if (!dst_in)
		return;

	xoff = left;
	yoff = top;

	while (size > 3) {
		xd = le16_to_cpu(*(int16_t *)(src + 0));
		src += 2;		// 52461
		xoff += xd;		// 52467
		b1 = *src++;		// 5246d - 5246f
		yoff += b1;		// 52470
		b2 = *src++;		// 486-488  EBP
		do {
			if (xoff > 0 && yoff > 0 && xoff < (ctx->rt.bufw - 1)) {
				if (yoff >= (ctx->rt.bufh - 1))
					return;

				dst = dst_in + xoff + (yoff * pitch);
				c1 = *(dst - 1) * 3; /* 0 - 765 */
				c2 = *(dst + 1) * 3;
				w1 = *(tbl1 + c1 + 0) + *(tbl1 + c2 + 0);
				w2 = *(tbl1 + c1 + 1) + *(tbl1 + c2 + 1);
				w3 = *(tbl1 + c1 + 2) + *(tbl1 + c2 + 2);
				c1 = *(dst - pitch) * 3;
				c2 = *(dst + pitch) * 3;
				w1 += *(tbl1 + c1 + 0) + *(tbl1 + c2 + 0);
				w2 += *(tbl1 + c1 + 1) + *(tbl1 + c2 + 1);
				w3 += *(tbl1 + c1 + 2) + *(tbl1 + c2 + 2);
				*dst = *(tbl2 + ((((w1 << 5) & 0x7c00) + (w2 & 0x3e0) + (w3 >> 5)) & 0x7fff));
			}
			xoff++;
			b2--;
		} while (b2 >= 0);
		xoff--;
		size -= 4;
	}
}

/******************************************************************************/

static void codec23(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
		    uint16_t h, int16_t top, int16_t left, uint16_t size,
		    uint8_t param, int16_t param2)
{
	const uint16_t mx = ctx->rt.bufw, my = ctx->rt.bufh, p = ctx->rt.pitch;
	int skip, i, j, ls, pc, y, wrlen, skip_left;
	uint8_t lut[256], *d;

	if (ctx->rt.version < 2) {
		/* RA1 */
		for (i = 0; i < 256; i++)
			lut[i] = (i + param + 0xd0) & 0xff;
	} else {
		/* RA2 FUN_00032350: first c23 has this LUT (param2 == 256),
		 * later frames reuse it (param2 == 257). param2 < 256 indicates
		 * this is a delta value to apply to the color instead.
		 */
		if (param2 == 256) {
			if (size < 256)
				return;
			memcpy(ctx->c23lut, src, 256);
			src += 256;
			size -= 256;
		} else if (param2 < 256) {
			/* create a lut with constant delta */
			for (i = 0; i < 256; i++)
				lut[i] = (i + param2) & 0xff;
		} else {
			for (i = 0; i < 256; i++)
				lut[i] = ctx->c23lut[i];
		}
	}

	if ((size < 1) || ((top + h) < 0) || (top >= my) || (left + w < 0) || (left >= mx))
		return;

	if (top < 0) {
		y = -top;
		while (y-- && size > 1) {
			ls = le16_to_cpu(ua16(src));
			size -= 2;
			if (size < ls)
				return;
			size -= ls;
			src += 2 + ls;
		}
		h += top;
		top = 0;
	}

	y = top;
	for (; (size > 1) && (h > 0) && (y < my); h--, y++) {
		ls = le16_to_cpu(ua16(src));
		src += 2;
		size -= 2;
		skip = 1;
		pc = left;
		while ((size > 0) && (ls > 0) && (pc <= (w + left))) {
			j = *src++;
			size--;
			ls--;
			if (!skip) {
				skip_left = (pc < 0) ? -pc : 0;
				if (skip_left >= j) {
					pc += j;
					j = 0;
				} else {
					pc += skip_left;
					j -= skip_left;
				}

				wrlen = (pc + j > mx) ? (mx - pc) : j;

				if (wrlen > 0) {
					d = dst + (y * p) + pc;
					for (i = 0; i < wrlen; i++) {
						d[i] = lut[d[i]];
					}

					pc += wrlen;
					j -= wrlen;
				}

				if (j > 0)
					pc += j;

			} else {
				pc += j;
			}
			skip ^= 1;
		}
	}
}

static void codec21(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
		    uint16_t h, int16_t top, int16_t left, uint16_t size,
		    uint8_t param)
{
	const uint16_t mx = ctx->rt.bufw, my = ctx->rt.bufh, p = ctx->rt.pitch;
	int j, y, pc, skip, ls;
	uint8_t c, *nsrc;

	if ((size < 1) || ((top + h) < 0) || (top >= my) || (left + w < 0) || (left >= mx))
		return;

	nsrc = src;
	y = top;
	for (; (size > 2) && (h > 0) && (y < my); y++, h--) {
		src = nsrc;
		ls = le16_to_cpu(ua16(src));
		src += 2;
		size -= 2;
		nsrc = src + ls;
		if (y < 0) {
			if (ls > size)
				break;
			size -= ls;
			continue;
		}
		skip = 1;
		pc = left;
		while ((size > 1) && (ls > 1) && (pc <= (w + left))) {
			j = le16_to_cpu(ua16(src));
			src += 2;
			size -= 2;
			ls -= 2;
			if (!skip) {
				while ((size-- > 0) && (ls-- > 0) && (j-- >= 0)) {
					c = *src++;
					if ((pc >= 0) && (pc < mx)) {
						*(dst + (y * p) + pc) = c;
					}
					pc++;
				}
			} else {
				pc += j;
			}
			skip ^= 1;
		}
	}
}

static void codec20(struct sanctx *ctx, uint8_t *dst, uint8_t *src, const uint16_t w,
		    const uint16_t h, int16_t top, int16_t left, uint32_t size,
		    uint16_t srcstride)
{
	if (((left + w) < 0) || (left >= ctx->rt.bufw) || ((top + h) < 0)
	    || (top >= ctx->rt.bufh) || (w < 1) || (h < 1))
		return;

	blt_solid(dst, src, left, top, 0, 0, w, h, srcstride, ctx->rt.pitch,
		  ctx->rt.bufh, size);
}

static void codec4_main(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
			uint16_t h, int16_t top, int16_t left, uint32_t size,
			uint8_t param, uint16_t param2, int c5)
{
	const uint16_t p = ctx->rt.pitch, mx = ctx->rt.bufw, my = ctx->rt.bufh;
	uint8_t mask, bits, idx, *gs, c4t;
	uint32_t dstoff;
	int i, j, k, l, bit, x, y;

	c4t = ctx->c4tblparam & 0xff;
	if (param2 > 0) {
		if (size < param2 * 8)
			return;
		c4_5_param2(ctx, src, param2, c4t);
		src += param2 * 8;
		size -= param2 * 8;
	}

	for (j = 0; j < w; j += 4) {
		mask = bits = 0;
		x = left + j;
		for (i = 0; i < h; i += 4) {
			y = top + i;
			if (param2 > 0) {
				if (bits == 0) {
					if (!size--)
						return;
					mask = *src++;
					bits = 8;
				}
				bit = !!(mask & 0x80);
				mask <<= 1;
				bits -= 1;
			} else {
				bit = 0;
			}

			if (!size--)
				return;
			idx = *src++;
			if (!bit && idx == 0x80 && !c5)
				continue;

			if ((y >= my) || ((y + 4) < 0) || ((x + 4) < 0) || (x >= mx))
				continue;
			/* render the 4x4 block */
			gs = &(ctx->c4tbl[bit][idx][0]);
			if ((y >= 0) && ((y + 4) < my) && (x >= 0) && ((x + 4) < mx)) {
				for (k = 0; k < 4; k++, gs += 4)
					memcpy(dst + x + (y + k) * p, gs, 4);
			} else {
				for (k = 0; k < 4; k++) {
					for (l = 0; l < 4; l++, gs++) {
						const int yo = y + k, xo = x + l;
						if ((yo >= 0) && (yo < my) && (xo >= 0) && (xo < mx))
							*(dst + yo * p + xo) = *gs;
					}
				}
			}

			/* post processing to smooth out block borders a bit.
			 * ASSAULT.EXE 121e8 - 12242 for the (c4t&0x80)==0 case.
			 */
			if (x <= 0 || y <= 0 || x >= mx || y >= my)
				continue;	/* skip unreachable edges */
			dstoff = y * p +  x;
			if (c4t & 0x80) {
				for (k = 0; k < 4; k++)
					*(dst + dstoff + k) = ((*(dst + dstoff + k) + *(dst + dstoff + k - p)) >> 1) | 0x80;
				*(dst + dstoff + 1 * p) = ((*(dst + dstoff + 1 * p) + *(dst + dstoff + 1 * p - 1)) >> 1) | 0x80;
				*(dst + dstoff + 2 * p) = ((*(dst + dstoff + 2 * p) + *(dst + dstoff + 2 * p - 1)) >> 1) | 0x80;
				*(dst + dstoff + 3 * p) = ((*(dst + dstoff + 3 * p) + *(dst + dstoff + 3 * p - 1)) >> 1) | 0x80;
			} else {
				for (k = 0; k < 4; k++)
					*(dst + dstoff + k) = ((*(dst + dstoff + k) + *(dst + dstoff + k - p)) >> 1) & 0x7f;
				*(dst + dstoff + 1 * p) = ((*(dst + dstoff + 1 * p) + *(dst + dstoff + 1 * p - 1)) >> 1) & 0x7f;
				*(dst + dstoff + 2 * p) = ((*(dst + dstoff + 2 * p) + *(dst + dstoff + 2 * p - 1)) >> 1) & 0x7f;
				*(dst + dstoff + 3 * p) = ((*(dst + dstoff + 3 * p) + *(dst + dstoff + 3 * p - 1)) >> 1) & 0x7f;
			}
		}
	}
}

static void codec33(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
		    uint16_t h, int16_t top, int16_t left, uint32_t size,
		    uint8_t param, uint16_t param2, int c5)
{
	if (ctx->c4tblparam != (param + 0x100))
		c33_34_tilegen(&(ctx->c4tbl[0][0][0]), param);
	ctx->c4tblparam = param + 0x100;
	codec4_main(ctx, dst, src, w, h, top, left, size, param, param2, c5);
}

static void codec4(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
		   uint16_t h, int16_t top, int16_t left, uint32_t size,
		   uint8_t param, uint16_t param2, int c5)
{
	if (ctx->c4tblparam != param)
		c4_5_tilegen(&(ctx->c4tbl[0][0][0]), param);
	ctx->c4tblparam = param;
	codec4_main(ctx, dst, src, w, h, top, left, size, param, param2, c5);
}

static void codec1(struct sanctx *ctx, uint8_t *dst_in, uint8_t *src, uint16_t w,
		   uint16_t h, int16_t top, int16_t left, uint32_t size, int transp)
{
	const uint16_t mx = ctx->rt.bufw, my = ctx->rt.bufh;
	uint8_t *dst, code, col;
	uint16_t rlen, dlen;
	int j, x, y;

	if (((top + h) < 0) || (top >= my) || (left + w < 0) || (left >= mx))
		return;
	if (top < 0) {
		y = -top;
		while (y-- && size > 1) {
			dlen = le16_to_cpu(ua16(src));
			size -= 2;
			if (size < dlen)
				return;
			size -= dlen;
			src += 2 + dlen;
		}
		h += top;
		top = 0;
	}

	y = top;
	for (; (size > 1) && (h > 0) && (y < my); h--, y++) {
		dlen = le16_to_cpu(ua16(src));
		src += 2;
		size -= 2;
		x = left;
		while (dlen && size) {
			code = *src++;
			dlen--;
			size--;
			rlen = (code >> 1) + 1;
			if (code & 1) {
				if (size < 1)
					return;
				col = *src++;
				dlen--;
				size--;
				if (x >= mx)
					continue;
				if (x < 0) {
					int dff = _min(-x, rlen);
					rlen -= dff;
					x += dff;
				}
				if (x + rlen > mx)
					rlen = mx - x;
				if (rlen < 1)
					continue;

				dst = (uint8_t *)dst_in + (y * ctx->rt.pitch) + x;
				if (col || !transp) {
					for (j = 0; j < rlen; j++)
						*(dst + j) = col;
				}
				x += rlen;
			} else {
				if (size < rlen)
					return;
				if (x >= mx) {
					dlen -= rlen;
					size -= rlen;
					src += rlen;
					continue;
				}
				if (x < 0) {
					int dff = _min(-x, rlen);
					src += dff;
					size -= dff;
					dlen -= dff;
					rlen -= dff;
					x += dff;
				}
				dst = (uint8_t *)dst_in + (y * ctx->rt.pitch) + x;
				for (j = 0; j < rlen; j++, x++) {
					col = *src++;
					if ((col || !transp) && (x >= 0) && (x < mx))
						*(dst + j) = col;
				}
				dlen -= rlen;
				size -= rlen;
			}
		}
	}
}

static void codec2(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
		   uint16_t h, int16_t top, int16_t left, uint32_t size,
		   uint8_t param, uint16_t param2)
{
	const uint16_t pitch = ctx->rt.pitch, maxx = ctx->rt.bufw, maxy = ctx->rt.bufh;
	int16_t xpos, ypos;

	/* RA2 31a10; but there are no codec2 fobjs in RA2 at all.. */
	if (param2 != 0 && ctx->rt.version == 2) {
		codec1(ctx, dst, src, w, h, top, left, size, 1);
		return;
	}

	/* ASSAULT.EXE 110f8 */
	xpos = left;	/* original:  - param7(xoff) */
	ypos = top;	/* original:  - param8(yoff) */
	while (size > 3) {
		xpos += (int16_t)le16_to_cpu(ua16(src));
		ypos += (int8_t)src[2];
		if (xpos >= 0 && ypos >= 0 && xpos < maxx && ypos < maxy) {
			*(dst + xpos + ypos * pitch) = src[3]; /* 110ff: pitch 320 */
		}
		src += 4;
		size -= 4;
	}
}

static void codec31(struct sanctx *ctx, uint8_t *dst, uint8_t *src, uint16_t w,
		    uint16_t h, int16_t top, int16_t left, uint32_t size, uint8_t p1,
		    int opaque)
{
	const uint16_t mx = ctx->rt.bufw, my = ctx->rt.bufh, p = ctx->rt.pitch;
	uint8_t code, col;
	uint16_t rlen, dlen;
	int j, x, y;

	if (((top + h) < 0) || (top >= my) || (left + w < 0) || (left >= mx))
		return;
	if (top < 0) {
		y = -top;
		while (y-- && size > 1) {
			dlen = le16_to_cpu(ua16(src));
			size -= 2;
			if (size < dlen)
				return;
			size -= dlen;
			src += 2 + dlen;
		}
		h += top;
		top = 0;
	}

	y = top;
	for (; (size > 1) && (h > 0) && (y < my); h--, y++) {
		dlen = le16_to_cpu(ua16(src));
		src += 2;
		size -= 2;
		x = left;
		while (dlen && size) {
			code = *src++;
			dlen--;
			size--;
			rlen = (code >> 1) + 1;
			if (code & 1) {
				if (size < 1)
					return;
				col = *src++;
				dlen--;
				size--;

				for (j = 0; j < rlen; j++) {
					uint8_t c1 = col & 0xf;
					if ((c1 || opaque) && (x >= 0) && (x < mx))
						*(dst + y * p + x) = p1 + c1;
					x++;
					c1 = col >> 4;
					if ((c1 || opaque) && (x >= 0) && (x < mx))
						*(dst + y * p + x) = p1 + c1;
					x++;
				}
			} else {
				if (size < rlen)
					rlen = size;
				for (j = 0; (j < rlen) && (size > 0); j++) {
					col = *src++;
					uint8_t c1 = col & 0xf;
					if ((c1 || opaque) && (x >= 0) && (x < mx))
						*(dst + y * p + x) = p1 + c1;
					x++;
					c1 = col >> 4;
					if ((c1 || opaque) && (x >= 0) && (x < mx))
						*(dst + y * p + x) = p1 + c1;
					x++;
				}
				dlen -= rlen;
				size -= rlen;
			}
		}
	}
}

/******************************************************************************/

static int handle_FOBJ(struct sanctx *ctx, uint32_t size, uint8_t *src, int16_t xoff, int16_t yoff)
{
	struct sanrt *rt = &ctx->rt;
	uint16_t w, h, wr, hr, param2;
	uint8_t codec, param, *dst;
	int16_t left, top;
	int ret, fsc;

	codec = src[0];
	param = src[1];

	left = le16_to_cpu(ua16(src + 2));
	top  = le16_to_cpu(ua16(src + 4));
	w = wr = le16_to_cpu(ua16(src + 6));
	h = hr = le16_to_cpu(ua16(src + 8));
	param2 = le16_to_cpu(ua16(src + 12));

	/* ignore nonsensical dimensions in frames, happens with
	 * some Full Throttle and RA2 videos.
	 * Do pass the data to codec45 though, it might have the tabledata in it!
	 */
	if ((w < 2) || (h < 2) || (w > FOBJ_MAXX) || (h > FOBJ_MAXY)) {
		if (codec == 45) {
			codec45(ctx, NULL, src + 14, 0, 0, 0, 0, size - 14, param, param2);
			return 0;
		}
		if (!rt->have_vdims)
			return 0;
	}

	/* codecs with their own front buffers */
	fsc = (codec == 37 || codec == 47 || codec == 48);

	/* SotE: all videos have top==60 to center the video in the
	 * 640x400 game window.  We don't need that.
	 */
	if ((w == 640) && (h == 272) && (top == 60) && (codec == 47))
		left = top = 0;

	/* decide on a buffer size */
	if (!rt->have_vdims) {
		if (rt->version < 2) {
			/* RA1: 384x242 internal buffer, 320x200 display */
			wr = 384;
			hr = 242;
			rt->have_vdims = 1;
			rt->bufw = wr;
			rt->bufh = hr;
			rt->pitch = wr;
		} else {
			/* detect common resolutions */
			wr = w + left;
			hr = h + top;
			if (((wr == 424) && (hr == 260)) ||	/* RA2 */
			    ((wr == 320) && (hr == 200)) ||	/* FT/DIG/.. */
			    ((wr == 640) && (hr == 272)) ||	/* SotE */
			    ((wr == 640) && (hr == 350)) ||	/* MotS */
			    ((wr == 640) && (hr == 480)) ||	/* COMI/OL/.. */
			    ((left == 0) && (top == 0) && (codec == 20) && (w > 3) && (h > 3))) {
				rt->have_vdims = 1;
			}

			rt->pitch = wr;
		}
		if (!rt->fbsize || (wr > rt->bufw) || (hr > rt->bufh)) {
			rt->bufw = _max(rt->bufw, wr);
			rt->bufh = _max(rt->bufh, hr);
			rt->fbsize = rt->bufw * rt->bufh * 1;
		}
	}

	if (rt->to_store == 2 || (rt->to_store != 0 && fsc)) {
		/* decode the image and change it to a FOBJ with codec20.
		 * Used sometimes in RA1 only; RA2+ had this feature removed.
		 * We can however use it for codecs37/47/48 since they work on
		 * their own buffers and don't modify existing images like the
		 * other codecs can do.
		 */
		*(uint32_t *)(rt->buf3 + 0) = rt->fbsize;/* block size in host endian */
		memcpy(rt->buf3 + 4, src, 14);		/* FOBJ header		*/
		*( uint8_t *)(rt->buf3 + 4) = 20;	/* set to codec20	*/
		dst = rt->buf3 + 4 + 14;		/* write image data here*/
		if (fsc) {
			/* for "Making Magic": copy the existing image too,
			 * change the codec20 to full buffer size
			 */
			memcpy(dst, rt->fbuf, rt->fbsize);
			*( uint16_t *)(rt->buf3 +  6) = 0;		/* left   */
			*( uint16_t *)(rt->buf3 +  8) = 0;		/* top    */
			*( uint16_t *)(rt->buf3 + 10) = rt->bufw;	/* width  */
			*( uint16_t *)(rt->buf3 + 12) = rt->bufh;	/* height */
		}
	} else {
		/* for codecs 1-34,44: default write to the front buffer */
		dst = rt->fbuf;
		rt->vbuf = dst;

		if (rt->to_store == 1) {
			/* copy the FOBJ whole (all SAN versions), and render it normally
			 * to the default front buffer.
			 */
			rt->to_store = 0;
			if (size <= rt->fbsize) {
				*(uint32_t *)rt->buf3 = size;
				memcpy(rt->buf3 + 4, src, size);
			} else {
				return 26;		/* STOR buffer too small! */
			}
		}
	}

	/* When it's the very first FOBJ in a FRME, and it's not one of the
	 * full-frame codecs, the frontbuffer needs to be cleared first.
	 */
	if (!rt->have_frame && rt->fbsize && !fsc) {
		memset(dst, 0, rt->fbsize);
	}

	src += 14;
	size -= 14;
	ret = 0;

	left += xoff;
	top += yoff;
	switch (codec) {
	case 1:
	case 3:   codec1(ctx, dst, src, w, h, top, left, size, (codec == 1)); break;
	case 2:   codec2(ctx, dst, src, w, h, top, left, size, param, param2); break;
	case 4:
	case 5:   codec4(ctx, dst, src, w, h, top, left, size, param, param2, codec == 5); break;
	case 20: codec20(ctx, dst, src, w, h, top, left, size, w); break;
	case 44:
	case 21: codec21(ctx, dst, src, w, h, top ,left, size, param); break;
	case 23: codec23(ctx, dst, src, w, h, top, left, size, param, param2); break;
	case 31:
	case 32: codec31(ctx, dst, src, w, h, top, left, size, param, codec == 32); break;
	case 33:
	case 34: codec33(ctx, dst, src, w, h, top, left, size, param, param2, codec == 34); break;
	case 45: codec45(ctx, dst, src, w, h, top, left, size, param, param2); break;
	case 37: ret = codec37(ctx, dst, src, w, h, top, left, size); break;
	case 47: ret = codec47(ctx, dst, src, w, h, top, left, size); break;
	case 48: ret = codec48(ctx, dst, src, w, h, top, left, size); break;
	default: ret = 18;
	}

	if (ret == 0) {
		/* the decoding-STOR was done, but we still need to put the image
		 * to the front buffer now.
		 */
		if (rt->to_store) {
			rt->to_store = 0;
			ret = handle_FOBJ(ctx, *(uint32_t *)rt->buf3, rt->buf3 + 4, 0, 0);
			if (ret)
				return ret;
		}

		ctx->rt.have_frame = 1;
	}

	rt->to_store = 0;

	return ret;
}

/* like c47_comp5 but adjusted for tbl2 lookups */
static void bl16_comp8(uint16_t *dst, uint8_t *src, uint32_t left, uint16_t *tbl2)
{
	uint8_t opc, rlen, j;
	uint16_t col;

	left >>= 1;	/* 16bit pixels */
	while (left) {
		opc = *src++;
		rlen = (opc >> 1) + 1;
		if (rlen > left)
			rlen = left;
		if (opc & 1) {
			col = le16_to_cpu(tbl2[*src++]);
			for (j = 0; j < rlen; j++)
				*dst++ = col;
		} else {
			for (j = 0; j < rlen; j++)
				*dst++ = le16_to_cpu(tbl2[*src++]);
		}
		left -= rlen;
	}
}

/* TGSMUSH.DLL 1000c690 */
static inline uint16_t bl16_c7_avg_col(uint16_t c1, uint16_t c2)
{
	return	(((c2 & 0x07e0) + (c1 & 0x07e0)) & 0x00fc0) |
		(((c2 & 0xf800) + (c1 & 0xf800)) & 0x1f000) |
		(((c2 & 0x001f) + (c1 & 0x001f))) >> 1;
}

/* this is basically codec47_comp1(), but for 16bit colors, with color averaging
 * instead of the interpolation table.
 * TGSMUSH.DLL c6f0
 */
static void bl16_comp7(uint16_t *dst, uint8_t *src, uint16_t w, uint16_t h,
		       uint16_t *tbl2)
{
	uint16_t hh, hw, c1, c2;
	uint8_t *dst1, *dst2;

	hh = (h + 1) >> 1;
	dst1 = (uint8_t *)(dst + (w * 2));
	while (hh--) {
		dst2 = dst1 + 4;
		c1 = le16_to_cpu(tbl2[*src++]);
		*(uint16_t *)(dst1 + 0) = c1;
		*(uint16_t *)(dst1 + 2) = c1;
		hw = (w - 1) >> 1;
		while (--hw) {
			c2 = le16_to_cpu(tbl2[*src++]);
			*(uint16_t *)dst2 = bl16_c7_avg_col(c1, c2);
			dst2 += 2;
			*(uint16_t *)dst2 = c2;
			dst2 += 2;
			c1 = c2;
		}
		dst1 += w * 2;	/* next line */
	}

	/* top row is a copy of 2nd row */
	memcpy(dst, dst + w * 2, w * 2);

	dst1 = (uint8_t *)(dst + (w * 4));
	hh = (h - 1) >> 1;
	while (hh--) {
		hw = w;				/* width is pixels! */
		while (hw--) {
			c1 = *(uint16_t *)(dst1 - (w * 2)); /* above */
			c2 = *(uint16_t *)(dst1 + (w * 2)); /* below */
			*(uint16_t *)dst1 = bl16_c7_avg_col(c1, c2);
			dst1 += 2;		/* 16 bit pixel */
		}
	}
}

/* TGSMUSH.DLL c0b4 */
static void bl16_comp6(uint16_t *dst, uint8_t *src, uint16_t w, uint16_t h,
		       uint16_t *tbl2)
{
	int i;
	for (i = 0; i < w * h; i++) {
		*dst++ = le16_to_cpu(tbl2[*src++]);
	}
}

/* TGSMUSH.DLL c5a0 */
static void bl16_comp1(uint16_t *dst, uint8_t *src, uint16_t w, uint16_t h)
{
	const uint32_t stride = 2 * w;
	uint8_t *dst1, *dst2;
	uint16_t hh, hw, c1, c2;

	if (h > 0) {
		hh = (h + 1) >> 1;
		dst1 = ((uint8_t *)dst) + stride;
		while (hh--) {
			c1 = le16_to_cpu(*(uint16_t *)src);
			src += 2;
			*(uint16_t *)(dst1 + 0) = c1;	/* first 2 pixels in row */
			*(uint16_t *)(dst1 + 2) = c1;			
			dst2 = dst1 + 4;		/* 2 16bit pixels */
			if (w - 2 > 0) {
				hw = (w - 1) >> 1;
				while (--hw) {
					c2 = le16_to_cpu(*(uint16_t *)src);
					src += 2;
					*(uint16_t *)dst2 = bl16_c7_avg_col(c1, c2);
					dst2 += 2;
					*(uint16_t *)dst2 = c2;
					dst2 += 2;
					c1 = c2;
				}
			}
			dst1 += 2 * stride;	/* start of 2nd next line */
		}

	}
	memcpy(dst, dst + stride, stride);
	dst1 = ((uint8_t *)dst) + (2 * stride);
	if (h - 2 > 0) {
		hh = (h - 1) >> 1;
		while (hh--) {
			hw = w;
			while (hw--) {
				c1 = *(uint16_t *)(dst1 + stride);
				c2 = *(uint16_t *)(dst1 - stride);
				*(uint16_t *)dst1 = bl16_c7_avg_col(c1, c2);
				dst1 += 2;	/* 1 16bit pixel */
			}
			dst1 += stride;
		}
	}
}

static uint8_t* bl16_block(uint8_t *src, uint8_t *dst, uint8_t *db1, uint8_t *db2,
			   uint16_t *tbl1, uint16_t *tbl2, uint16_t w,
			   uint32_t stride, uint8_t blksize, struct sanctx *ctx)
{
	int32_t mvofs, ofs;
	int8_t *pglyph;
	uint16_t c[2];
	uint8_t opc;
	int16_t o2;
	int i, j;

	opc = *src++;
	switch (opc) {
	case 0xff:
		if (blksize == 2) {
			*(uint16_t *)(dst + 0      + 0) = le16_to_cpu(ua16(src));
			src += 2;
			*(uint16_t *)(dst + 0      + 2) = le16_to_cpu(ua16(src));
			src += 2;
			*(uint16_t *)(dst + stride + 0) = le16_to_cpu(ua16(src));
			src += 2;
			*(uint16_t *)(dst + stride + 2) = le16_to_cpu(ua16(src));
			src += 2;
		} else {
			src = bl16_block(src, dst, db1, db2, tbl1, tbl2,
					 w, stride, blksize >> 1, ctx);
			src = bl16_block(src, dst + blksize, db1 + blksize, db2 + blksize,
					 tbl1, tbl2, w, stride, blksize >> 1, ctx);
			dst += stride * (blksize >> 1);
			db1 += stride * (blksize >> 1);
			db2 += stride * (blksize >> 1);
			src = bl16_block(src, dst, db1, db2, tbl1, tbl2,
					 w, stride, blksize >> 1, ctx);
			src = bl16_block(src, dst + blksize, db1 + blksize, db2 + blksize,
					 tbl1, tbl2, w, stride, blksize >> 1, ctx);
		}
		break;
	case 0xfe:
		/* fill a block with a color value from the stream */
		c[0] = le16_to_cpu(ua16(src));
		src += 2;
		for (i = 0; i < blksize; i++) {
			ofs = i * stride;
			for (j = 0; j < blksize; j++) {
				*(uint16_t *)(dst + ofs + j*2) = c[0];
			}
		}
		break;
	case 0xfd:
		/* fill a block using tbl2 color, index from next byte */
		c[0] = le16_to_cpu(tbl2[*src++]);
		for (i = 0; i < blksize; i++) {
			ofs = i * stride;
			for (j = 0; j < blksize; j++) {
				*(uint16_t *)(dst + ofs + j*2) = c[0];
			}
		}
		break;
	case 0xfc:
	case 0xfb:
	case 0xfa:
	case 0xf9:
		/* fill a block using tbl1 color */
		c[0] = le16_to_cpu(tbl1[(opc - 0xf9)]);
		for (i = 0; i < blksize; i++) {
			ofs = i * stride;
			for (j = 0; j < blksize; j++) {
				*(uint16_t *)(dst + ofs + j*2) = c[0];
			}
		}
		break;
	case 0xf8:
		if (blksize == 2) {
			*(uint16_t *)(dst + 0      + 0) = le16_to_cpu(ua16(src));
			src += 2;
			*(uint16_t *)(dst + 0      + 2) = le16_to_cpu(ua16(src));
			src += 2;
			*(uint16_t *)(dst + stride + 0) = le16_to_cpu(ua16(src));
			src += 2;
			*(uint16_t *)(dst + stride + 2) = le16_to_cpu(ua16(src));
			src += 2;
		} else {
			opc = *src++;
			c[1] = le16_to_cpu(ua16(src));
			src += 2;
			c[0] = le16_to_cpu(ua16(src));
			src += 2;
			pglyph = (blksize == 8) ? ctx->c47_glyph8x8[opc] : ctx->c47_glyph4x4[opc];
			for (i = 0; i < blksize; i++) {
				ofs = i * stride;
				for (j = 0; j < blksize; j++) {
					*(uint16_t *)(dst + ofs + j*2) = c[(*pglyph++)];
				}
			}
		}
		break;
	case 0xf7:
		if (blksize == 2) {
			*(uint16_t *)(dst + 0      + 0) = le16_to_cpu(tbl2[*src++]);
			*(uint16_t *)(dst + 0      + 2) = le16_to_cpu(tbl2[*src++]);
			*(uint16_t *)(dst + stride + 0) = le16_to_cpu(tbl2[*src++]);
			*(uint16_t *)(dst + stride + 2) = le16_to_cpu(tbl2[*src++]);
		} else {
			opc = *src++;
			c[1] = le16_to_cpu(tbl2[*src++]);
			c[0] = le16_to_cpu(tbl2[*src++]);
			pglyph = (blksize == 8) ? ctx->c47_glyph8x8[opc] : ctx->c47_glyph4x4[opc];
			for (i = 0; i < blksize; i++) {
				ofs = i * stride;
				for (j = 0; j < blksize; j++) {
					*(uint16_t *)(dst + ofs + j*2) = c[!!(*pglyph++)];
				}
			}
		}
		break;
	case 0xf6:	/* copy from db1 at same spot */
		for (i = 0; i < blksize; i++) {
			ofs = i * stride;
			for (j = 0; j < blksize; j++) {
				*(uint16_t *)(dst + ofs + j*2) = *(uint16_t *)(db1 + ofs + j*2);
			}
		}
		break;
	case 0xf5:	/* copy from db2, mvec from source */
		o2 = le16_to_cpu((int16_t)ua16(src));
		src += 2;
		mvofs = o2 * 2;  /* since stride = w*2 */
		for (i = 0; i < blksize; i++) {
			ofs = i * stride;
			for (j = 0; j < blksize; j++) {
				*(uint16_t *)(dst + ofs + j*2) = *(uint16_t *)(db2 + ofs + j*2 + mvofs);
			}
		}

		break;
	default:
		/* opc is index into c47 mv table, copy 8x8 block from db2.
		 * IMPORTANT: with width 800, for opc 1-4, the calculation will
		 * overflow the int16, turning the large negative values into
		 * large positive values.  This is by design, and exploited by
		 * the 800x600 jonesopn_8.snm video from "Indiana Jones and
		 *  the Infernal Machine".
		 * tl;dr: the cast to int16_t is essential for this to work.
		 * But signed integer overflow is UB according to C standard,
		 *  so we first calculate an int32_t, cast to an uint16_t (which
		 *  is not UB) to truncate, then cast to signed int16_t.
		 */
		mvofs = (c47_mv[opc][1] * w + c47_mv[opc][0]);	/* not overflowing */
		uint16_t u16 = (uint16_t)mvofs;			/* truncate, no UB */
		int16_t i16 = (int16_t)u16;
		mvofs = i16 * 2;
		for (i = 0; i < blksize; i++) {
			ofs = i * stride;
			for (j = 0; j < blksize; j++) {
				*(uint16_t *)(dst + ofs + j*2) = *(uint16_t *)(db2 + ofs + j*2 + mvofs);
			}
		}
		break;
	}
	return src;
}

static void bl16_comp2(uint8_t *dst, uint8_t *src, uint16_t w, uint16_t h,
		       uint8_t *db1, uint8_t *db2, uint16_t *tbl1, uint16_t *tbl2,
		       struct sanctx *ctx)
{
	const uint32_t stride = w * 2;
	int i, j;

	h = (h + 7) & ~7;
	w = (w + 7) & ~7;

	for (j = 0; j < h; j += 8) {
		for (i = 0; i < 2 * w; i += 8 * 2) {
			src = bl16_block(src, dst + i, db1 + i , db2 + i, tbl1,
					 tbl2, w, stride, 8, ctx);
		}
		dst += stride * 8;
		db1 += stride * 8;
		db2 += stride * 8;
	}
}

static void handle_BL16(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	struct sanrt *rt = &ctx->rt;
	uint16_t *dst, *db1, *db2, width, height, seq;
	uint16_t *tbl1, *tbl2, bgc;
	uint8_t codec, newrot;
	uint32_t decsize;
	int i;

	if (size < 0x230)
		return;

	dst = (uint16_t *)rt->buf0;
	db1 = (uint16_t *)rt->buf1;
	db2 = (uint16_t *)rt->buf2;

	width  = le16_to_cpu(*(uint16_t *)(src + 8));
	height = le16_to_cpu(*(uint16_t *)(src + 12));
	seq    = le16_to_cpu(*(uint16_t *)(src + 16));
	codec  = src[18];
	newrot = src[19];
	tbl1 = (uint16_t *)(src + 24);
	bgc = le16_to_cpu(*(uint16_t *)(src + 32));
	decsize = le32_to_cpu(*(uint32_t *)(src + 36));
	tbl2 = (uint16_t *)(src + 40);

	if (seq == 0) {
		rt->lastseq = -1;
		for (i = 0; i < width * height; i++) {
			*db1++ = bgc;
			*db2++ = bgc;
		}
		db1 = (uint16_t *)rt->buf1;
		db2 = (uint16_t *)rt->buf2;
	}

	src += 0x230;
	size -= 0x230;
	switch (codec) {
	case 0: for (i = 0; i < width * height; i++, src += 2)
			*dst++ = le16_to_cpu(*(uint16_t *)src);
		break;
	case 1: bl16_comp1(dst, src, width, height); break;
	case 2: if (seq == rt->lastseq + 1)
			bl16_comp2((uint8_t *)dst, src, width, height,
				   (uint8_t *)db1, (uint8_t *)db2, tbl1, tbl2, ctx);
		break;
	case 3:	memcpy(dst, db2, width * height * 2); break;
	case 4: memcpy(dst, db1, width * height * 2); break;
	case 5: codec47_comp5(src, size, (uint8_t *)dst, decsize); break;
	case 6: bl16_comp6(dst, src, width, height, tbl2); break;
	case 7: bl16_comp7(dst, src, width, height, tbl2); break;
	case 8: bl16_comp8(dst, src, decsize, tbl2); break;
	}

	rt->vbuf = rt->buf0;
	rt->have_frame = 1;
	rt->palette = NULL;
	if (seq == rt->lastseq + 1)
		c47_swap_bufs(ctx, newrot);
	rt->lastseq = seq;
}

static void handle_NPAL(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	if (size >= 768)
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
	uint32_t *pal = ctx->rt.palette;
	int16_t *sp = ctx->rt.shiftpal;
	int16_t *dp = ctx->rt.deltapal;
	int i, j, t2[3];
	uint16_t cmd;

	if (size < 4)
		return;

	cmd = be16_to_cpu(*(uint16_t *)(src + 2));
	src += 4;

	if (cmd == 0 || cmd == 2) {
		if (cmd == 2) {
			if (size < (768 * 3 + 4))
				return;
			read_palette(ctx, src + (768 * 2));
		}
		if (size < (768 * 2 + 4))
			return;

		for (i = 0; i < 768; i += 3) {
			dp[i + 0] = le16_to_cpu(*(int16_t *)(src + 0));
			dp[i + 1] = le16_to_cpu(*(int16_t *)(src + 2));
			dp[i + 2] = le16_to_cpu(*(int16_t *)(src + 4));
			src += 6;
			sp[i + 0] = ((*pal >>  0) & 0xff) << 7;
			sp[i + 1] = ((*pal >>  8) & 0xff) << 7;
			sp[i + 2] = ((*pal >> 16) & 0xff) << 7;
			pal++;
		}
	} else {
		for (i = 0; i < 768; i += 3) {
			for (j = 0; j < 3; j++) {
				sp[i + j] += dp[i + j];
				t2[j] = _u8clip(sp[i + j] >> 7);
			}
			*pal++ = 0xffU << 24 | t2[2] << 16 | t2[1] << 8 | t2[0];
		}
	}
}

/*******************************************************************************
 *
 *  AUDIO
 *
 ******************************************************************************/

static void atrk_setdamp(struct sanmsa *msa, uint16_t dampmin, uint16_t dampmax,
			 uint16_t diprate, uint16_t riserate)
{
	if (dampmax > 256)
		dampmax = 256;		/* FT default */
	if (dampmin > dampmax)
		dampmin = 114;
	if (dampmax < dampmin)
		dampmax = 256;
	if (riserate > 256 || riserate < 1)
		riserate = 32;
	if (diprate > 256 || diprate < 1)
		diprate = 16;

	msa->sou_vol_damp = dampmax;
	msa->sou_damp_min = dampmin;
	msa->sou_damp_max = dampmax;
	msa->sou_damp_dip_rate = diprate;
	msa->sou_damp_rise_rate = riserate;
}

static void atrk_init_volumes(struct sanmsa *msa)
{
	msa->sou_vol_sfx = 127;
	msa->sou_vol_voice = 127;
	msa->sou_vol_music = 127;
	msa->sou_vol_global = 127;
	atrk_setdamp(msa, 114, 256, 16, 48);
}

static uint32_t atrk_bytes_to_dstframes(struct sanatrk *atrk, uint32_t avail)
{
	uint32_t den = atrk->src_cnvrate >> 8;

	if (atrk->flags & ATRK_SRC8BIT) {
		avail = avail * 1;		/* samples */
	} else if (atrk->flags & ATRK_SRC12BIT) {
		avail = (avail * 2) / 3;	/* samples */
	} else {
		avail = avail >> 1;		/* samples */
	}
	if (0 == (atrk->flags & ATRK_1CH)) {
		avail >>= 1;			/* frames */
	}

	if (den == 0) {
		avail = (avail == 0) ? 0 : 4096;
	} else {
		uint32_t num = avail << 8;
		if (num > (4096 * den))
			avail = 4096;
		else
			avail = num / den;
	}
	return avail;
}

static void atrk_update_dstframes_avail(struct sanatrk *atrk)
{
	atrk->dstfavail = atrk_bytes_to_dstframes(atrk, atrk->datacnt);
	atrk->dstpavail = atrk_bytes_to_dstframes(atrk, atrk->playlen);
}

/* extract a sample for channel "ch" from frame at offset "frameofs" in the source
 * at the readpointer.  For 16bit-LE source data format.
 */
static int16_t atrk_decode_src16le(struct sanatrk *atrk, uint32_t frameofs, uint8_t ch)
{
	const uint32_t chm = (atrk->flags & ATRK_1CH) ? 1 : 2;	/* channel mult */
	uint32_t srcofs;
	uint8_t lo, hi;

	srcofs = (atrk->rdptr + (frameofs * chm * 2) + (ch * 2));
	srcofs &= ATRK_DATMASK;
	lo = atrk->data[srcofs];
	hi = atrk->data[(srcofs + 1) & ATRK_DATMASK];
	return (int16_t)((hi << 8) | lo);
}

/* extract a sample for channel "ch" from frame at offset "frameofs" in the source
 * at the readpointer.  For unsigned 8bit source data format.
 */
static int16_t atrk_decode_src8(struct sanatrk *atrk, uint32_t frameofs, uint8_t ch)
{
	const uint32_t chm = (atrk->flags & ATRK_1CH) ? 1 : 2;	/* channel mult */
	uint32_t srcofs;

	srcofs = (atrk->rdptr + (frameofs * chm * 1) + (ch * 1));
	srcofs &= ATRK_DATMASK;
	return ((atrk->data[srcofs]) << 8) ^ 0x8000;
}

/* extract a sample for channel "ch" from frame at offset "frameofs" in the source
 * at the readpointer.  For packed 12bit source data format.
 */
static int16_t atrk_decode_src12(struct sanatrk *atrk, uint32_t frameofs, uint8_t ch)
{
	const uint32_t ch1 = atrk->flags & ATRK_1CH;
	uint8_t b0, b1 ,b2;
	uint32_t srcofs;
	int16_t s[2];

	/*   mono: 3 src bytes for 2 16bit frames.
	 * stereo: 3 src bytes for 1 16bit-2ch frame */
	srcofs = atrk->rdptr + (ch1 ? (frameofs / 2) * 3 : frameofs * 3);
	b0 = atrk->data[(srcofs + 0) & ATRK_DATMASK];
	b1 = atrk->data[(srcofs + 1) & ATRK_DATMASK];
	b2 = atrk->data[(srcofs + 2) & ATRK_DATMASK];
	s[0] = ((((b1 & 0x0f) << 8) | b0) << 4) - 0x8000;
	s[1] = ((((b1 & 0xf0) << 4) | b2) << 4) - 0x8000;

	return (ch1) ? s[frameofs & 1] : s[!!ch];
}

static inline void atrk_reset(struct sanatrk *atrk)
{
	/* don't overwrite 1st and last member! */
	memset(&atrk->rdptr, 0, sizeof (struct sanatrk) - 2*sizeof(void *));
}

static inline void atrk_set_srate(struct sanatrk *atrk, uint32_t rate)
{
	atrk->srate = rate;
	atrk->src_cnvrate = (rate << 16) / (22050);
}

static inline void atrk_set_playpos(struct sanatrk *atrk, uint32_t ofs, uint32_t len)
{
	atrk->rdptr = ofs & ATRK_DATMASK;
	atrk->playlen = len;
	atrk->src_accum = 0;
	atrk->dstpavail = atrk_bytes_to_dstframes(atrk, atrk->playlen);
	atrk->flags |= ATRK_REMIX;
}

/* create a default STRK script which plays the whole stream, then terminates,
 * for the iMUSE (The Dig) tracks.
 */
static inline void atrk_set_default_strk(struct sanatrk *atrk, uint32_t len)
{
	uint8_t *stp = atrk->strk;

	/* set cmd1, play offset 0, play length 'len' */
	stp[0] = 1;
	stp[1] = 8;
	stp[2] = stp[3] = stp[4] = stp[5] = 0;
	stp[6] = (len >> 24) & 0xff;
	stp[7] = (len >> 16) & 0xff;
	stp[8] = (len >>  8) & 0xff;
	stp[9] = (len >>  0) & 0xff;
	/* cmd5: terminate stream */
	stp[10] = 5;
	atrk->strksz = 11;
}

/* STRK script processor, called when a stream has run out of data to play. */
static void atrk_process_strk(struct sanatrk *atrk)
{
	uint8_t *s, *r = &atrk->msa->sou_hooks[0xff];
	uint32_t v1, v2, v3, v4;
	int j;

	j = atrk->strksz - atrk->strkptr;
	if (j < 1) {
		atrk_reset(atrk);
		return;
	}

	do {
		s = atrk->strk + atrk->strkptr;
		switch (s[0]) {
		case 1:					/* set play offset + length */
			if (j < 10)
				goto out_err;

			v1 = be32_to_cpu(ua32(s + 2 + 0));
			v2 = be32_to_cpu(ua32(s + 2 + 4));
			atrk_set_playpos(atrk, v1, v2);
			atrk->strkptr += s[1] + 2;
			atrk->af4 = 0;			/* for case 7 */
			goto out_ok;			/* DONE: playlen set */
		case 3:					/* set channel params/hooks */
			if (j < 4)
				goto out_err;

			if (s[2] == 0xff) {
				*r = s[3];
			} else if (s[2] == 0xfe) {
				if ((s[3] >= 0) && (s[3] < 128))
					atrk->vol = s[3];
			} else if (s[2] == 0xfd) {
				if (((int8_t)s[3] > -128) && (s[3] < 128))
					atrk->pan = s[3];
			} else {
				atrk->msa->sou_hooks[s[2]] = s[3];
			}
			atrk->strkptr += s[1] + 2;
			break;
		case 4:					/* change channel params */
			if (j < 4)
				goto out_err;

			if (s[2] == 0xff) {
				*r += (int8_t)s[3];	/* "register" */
			} else if (s[2] == 0xfe) {
				atrk->vol += (int8_t)s[3];
				if (atrk->vol > 127)
					atrk->vol = 127;
			} else if (s[2] == 0xfd) {
				atrk->pan += (int8_t)s[3];
				if (atrk->pan & 0x80)
					atrk->pan = 0;
			} else {
				atrk->msa->sou_hooks[s[2]] += (int8_t)s[3];
			}
			atrk->strkptr += s[1] + 2;
			break;
		case 6:			/* set play offset, length, samplerate */
			if (j < 14)
				goto out_err;

			v1 = be32_to_cpu(ua32(s + 2 + 0));
			v2 = be32_to_cpu(ua32(s + 2 + 4));
			v3 = be32_to_cpu(ua32(s + 2 + 8));
			if ((v3 < 1000) || (v3 > 48000))
				goto out_err;
			atrk_set_playpos(atrk, v1, v2);
			atrk_set_srate(atrk, v3);
			atrk->strkptr += s[1] + 2;
			atrk->af4 = 0;			/* for case 7 */
			goto out_ok;			/* DONE: playlen set */
		case 2:
		case 8:
		case 9:
		case 10:
		case 11:	/* advance STRK ptr based on params + op */
			if (j < 6)
				goto out_err;

			/* gather params */
			if (s[4] == 0xff) {
				/* do nothing: *r = *r */
			} else if (s[4] == 0xfe) {
				*r = atrk->vol;
			} else if (s[4] == 0xfd) {
				*r = atrk->pan;
			} else {
				*r = atrk->msa->sou_hooks[s[4]];
			}
			/* execute logical op */
			switch (s[0]) {
			case 2:	 *r =((*r == 0) && (s[4] != 0)) ? 0 : 1; break;
			case 8:  *r = (s[5] < *r) ? 1 : 0; break;
			case 9:  *r = (*r < s[5]) ? 1 : 0; break;
			case 10: *r = (*r == s[5]) ? 1 : 0; break;
			case 11: *r = (*r == s[5]) ? 0 : 1; break;
			}
			if (*r == 0) {
				atrk->strkptr += s[1] + 2;
			} else {
				atrk->strkptr += (int16_t)be16_to_cpu(ua16(s + 2));
			}
			break;
		case 7:		/* XXX: not exactly sure yet */
			if (j < 20)
				goto out_err;

			v1 = be32_to_cpu(ua32(s + 2 +  0));
			v2 = be32_to_cpu(ua32(s + 2 +  4));
			v3 = be32_to_cpu(ua32(s + 2 +  8));
			v4 = be32_to_cpu(ua32(s + 2 + 12));
			if ((v3 < 1000) || (v3 > 48000))
				goto out_err;
			/* SHELL.EXE 41cb5e+ */
			if (atrk->af4 == 0) {
				atrk->af4 = v2;
				atrk->af0 = 0;
			}
			if (atrk->af4 <= v4)
				v4 = atrk->af4;
			atrk_set_playpos(atrk, v1 + atrk->af0, v4);
			atrk_set_srate(atrk, v3);
			atrk->af4 -= v4;
			atrk->af0 += v4;
			if (atrk->af4 == 0)
				atrk->strkptr += (int16_t)be16_to_cpu(ua16(s + 2 + 16));
			else
				atrk->strkptr += s[1] + 2;
			goto out_ok;			/* DONE: playlen set */
		default:
		case 5:					/* reset channel */
			atrk_reset(atrk);
			goto out_ok;
		}

		j = atrk->strksz - atrk->strkptr;
	} while (j > 0);
	/* no more STRK bytes, but still here? Just terminate the stream then */
out_err:
	atrk_reset(atrk);
out_ok:
	return;
}

static void atrk_set_srcfmt(struct sanatrk *atrk, uint16_t rate,
			    uint8_t bits, uint8_t ch, uint8_t vol, int8_t pan)
{
	atrk->flags &= ~(ATRK_1CH | ATRK_SRC8BIT | ATRK_SRC12BIT);
	if (bits == 8) {
		atrk->flags |= ATRK_SRC8BIT;
		atrk->decode = atrk_decode_src8;
	} else if (bits == 12) {
		atrk->flags |= ATRK_SRC12BIT;
		atrk->decode = atrk_decode_src12;
	} else {
		atrk->decode = atrk_decode_src16le;
	}
	if (ch < 2)
		atrk->flags |= ATRK_1CH;

	atrk->vol = vol;
	atrk->pan = pan;
	atrk_set_srate(atrk, rate);
}

/* find a trkid in the santrak list. Default allocate a new if the trkid is
 * not yet valid, or optionally return NULLptr if trkid is unknown.
 */
static struct sanatrk *atrk_find_trkid(struct sanmsa *msa, uint16_t trkid,
				       int fail_on_not_found)
{
	struct sanatrk *atrk;
	int i, newid;

	newid = -1;
	for (i = 0; i < msa->numtrk; i++) {
		atrk = &(msa->atrk[i]);
		if ((atrk->flags & ATRK_INUSE) && (trkid == atrk->trkid))
			return atrk;
		if ((newid < 0) && (0 == (atrk->flags & ATRK_INUSE)))
			newid = i;
	}
	if ((newid > -1) && (!fail_on_not_found)) {
		atrk = &(msa->atrk[newid]);
		atrk_reset(atrk);
		atrk->vol = 127;
		return atrk;
	}
	return NULL;
}

/* clear the ATRK_MIXED flag from all tracks */
static void atrk_reset_mixed(struct sanmsa *msa)
{
	for (int i = 0; i < msa->numtrk; i++)
		msa->atrk[i].flags &= ~ATRK_MIXED;
}

/* get the FIRST mixable track. */
static struct sanatrk *atrk_get_next_mixable(struct sanmsa *msa)
{
	struct sanatrk *atrk;
	for (int i = 0; i < msa->numtrk; i++) {
		atrk = &(msa->atrk[i]);
		if ((ATRK_INUSE == (atrk->flags & (ATRK_INUSE | ATRK_MIXED | ATRK_BLOCKED)))
			&& atrk->dstfavail)
			return atrk;
	}
	return NULL;
}

/* count all mixable tracks, with shortest buffer size returned too */
static int atrk_count_mixable(struct sanmsa *msa, uint32_t *minlen)
{
	struct sanatrk *atrk;
	int i, mixable;
	uint32_t ml, df;

	ml = ~0U;
	mixable = 0;
	for (i = 0; i < msa->numtrk; i++) {
		atrk = &(msa->atrk[i]);
		df = _min(atrk->dstpavail, atrk->dstfavail);
		if ((ATRK_INUSE == (atrk->flags & (ATRK_INUSE | ATRK_MIXED | ATRK_BLOCKED))) && df) {
			mixable++;
			if (ml > df) {
				ml = df;
			}
		}
	}

	if (minlen)
		*minlen = ml;
	return mixable;
}

/* count all active tracks: tracks which are still allocated */
static int atrk_count_active(struct sanmsa *msa, int *voice)
{
	struct sanatrk *atrk;
	int i, active;

	active = 0;
	if (voice)
		*voice = 0;
	for (i = 0; i < msa->numtrk; i++) {
		atrk = &(msa->atrk[i]);
		if (voice && (SAUD_FLAG_TRK_VOICE == (atrk->pflags & SAUD_FLAG_TRK_MASK)))
			*voice = 1;
		if (ATRK_INUSE == (atrk->flags & (ATRK_INUSE)))
			active++;
	}
	return active;
}

static inline void atrk_finish_all(struct sanmsa *msa)
{
	struct sanatrk *atrk;
	int i;

	for (i = 0; i < msa->numtrk; i++) {
		atrk = &(msa->atrk[i]);
		if (0 == (atrk->flags & ATRK_INUSE))
			continue;
		if ((atrk->playlen / atrk->srate) > 60)
			atrk_reset(atrk);
		else
			atrk->strksz = 0;
	}
}

/* buffer the incoming data to the ringbuffer */
static void atrk_read_pcmsrc(struct sanatrk *atrk, uint32_t size, uint8_t *src)
{
	uint32_t toend;

	toend = ATRK_DATSZ - atrk->wrptr;
	if (size <= toend) {
		memcpy(atrk->data + atrk->wrptr, src, size);
	} else {
		memcpy(atrk->data + atrk->wrptr, src, toend);
		memcpy(atrk->data + 0, src + toend, size - toend);
	}
	atrk->datacnt += size;
	atrk->wrptr += size;
	atrk->wrptr &= ATRK_DATMASK;
	atrk_update_dstframes_avail(atrk);
}

static void atrk_consume(struct sanatrk *atrk, uint32_t bytes)
{
	atrk->rdptr += bytes;
	atrk->rdptr &= ATRK_DATMASK;
	if (bytes > atrk->playlen)
		bytes = atrk->playlen;
	atrk->playlen -= bytes;
	atrk_update_dstframes_avail(atrk);
	if (atrk->dstpavail < 1)
		atrk_process_strk(atrk);
}

static int atrk_frame_data_avail(struct sanatrk *atrk, uint32_t frameidx)
{
	const uint32_t chm = (atrk->flags & ATRK_1CH) ? 1 : 2;
	uint32_t n1, n2;
	int ret = 0;

	if (atrk->flags & ATRK_SRC8BIT) {
		n1 = (frameidx + 1) * chm;
		n2 = (frameidx + 2) * chm;
	} else if (atrk->flags & ATRK_SRC12BIT) {
		if (atrk->flags & ATRK_1CH) {
			n1 = (((frameidx + 0) / 2) + 1) * 3;
			n2 = (((frameidx + 1) / 2) + 1) * 3;
		} else {
			n1 = ((frameidx + 0) + 1) * 3;
			n2 = ((frameidx + 1) + 1) * 3;
		}
	} else {
		n1 = ((frameidx + 0) + 1) * chm * 2;
		n2 = ((frameidx + 1) + 1) * chm * 2;
	}
	ret  = (n1 <= atrk->datacnt) ? 1 : 0;
	ret |= (n2 <= atrk->datacnt) ? 2 : 0;
	return ret;
}

/* convert the ATRK src data to 16bit 2ch and resample it to get the desired
 * number of target frames.
 */
static void atrk_convert_resample(struct sanatrk *atrk, int16_t *dst,
				  uint32_t dest_frame_count)
{
	const uint32_t chm = (atrk->flags & ATRK_1CH) ? 1 : 2;
	uint32_t isidx, frac, tc, bc;
	int32_t l, r, s1, s2, s3, s4;
	int fda;

	for (uint32_t i = 0; i < dest_frame_count; i++) {
		/* input sample real index and fractional part */
		isidx = atrk->src_accum >> 16;
		frac = atrk->src_accum & 0xFFFF;

		/* get info about source availability of current and next frame.
		 * bitmask: bit 1 = have source data at index frame,
		 * bit 2 = have source data at next index frame.
		 */
		fda = atrk_frame_data_avail(atrk, isidx);

		if (!(fda & 1)) {
			/* no CURRENT data, this should not happen, but
			 * generate silence.
			 */
			dst[i * 2 + 0] = 0;
			dst[i * 2 + 1] = 0;
		} else {
			if (atrk->flags & ATRK_1CH) {
				/* mono: get 2 samples if possible, then inter-
				 * polate between them and write it to both
				 * output channels.
				 */
				s1 = atrk->decode(atrk, isidx, 0);
				s2 = (fda & 2) ? atrk->decode(atrk, isidx + 1, 0) : s1;
				l = r = s1 + (((s2 - s1) * (int32_t)frac) >> 16);
			} else {
				/* stereo: get a sample for both channels */
				s1 = atrk->decode(atrk, isidx, 0);
				s2 = atrk->decode(atrk, isidx, 1);

				if (fda & 2) {
					/* have next frame: get it an interpolate */
					s3 = atrk->decode(atrk, isidx + 1, 0);
					s4 = atrk->decode(atrk, isidx + 1, 1);

					l = s1 + (((s3 - s1) * (int32_t)frac) >> 16);
					r = s2 + (((s4 - s2) * (int32_t)frac) >> 16);
				} else {
					/* no next frame: duplicate existing data */
					l = s1;
					r = s2;
				}
			}

			/* clamp */
			if (l > 32767)
				l = 32767;
			if (l < -32768)
				l = -32768;
			if (r > 32767)
				r = 32767;
			if (r < -32768)
				r = -32768;

			/* write */
			dst[i * 2 + 0] = (int16_t)l;
			dst[i * 2 + 1] = (int16_t)r;
		}

		/* advance input sample position */
		atrk->src_accum += atrk->src_cnvrate;
	}

	/* update track pointers/counters, need to calculate how many bytes of
	 * the INPUT format have been consumed.
	 */
	tc = atrk->src_accum >> 16;	/* how many source frames read */
	if (atrk->flags & ATRK_SRC8BIT) {
		bc = tc * chm;		/* bytes = frames * channels */
	} else if (atrk->flags & ATRK_SRC12BIT) {
		if (atrk->flags & ATRK_1CH) {
			bc = ((tc + 1) / 2) * 3;	/* 3 bytes per 2 frames */
		} else {
			bc = tc * 3;	/* 3 bytes per frame */
		}
	} else {
		bc = tc * 2 * chm;	/* 2bytes per frame * channels */
	}
	atrk_consume(atrk, bc);
	/* we've advanced rdptr, keep only the fractional part of accumulator */
	atrk->src_accum &= 0x0000ffff;
}

static void aud_mixs16(uint8_t *ds1, uint8_t *s1, uint8_t *s2, int bytes,
		       uint8_t vol1, int8_t pan1, uint8_t vol2, int8_t pan2)
{
	int32_t vol1_l, vol1_r, vol2_l, vol2_r;
	int16_t *src1 = (int16_t *)s1;
	int16_t *src2 = (int16_t *)s2;
	int16_t *dst = (int16_t *)ds1;
	int d1, d2, d3;

	if (pan1 == 0) {
		vol1_l = vol1_r = vol1;
	} else if (pan1 < 0) {
		vol1_l = vol1;
		vol1_r = (vol1 * (ATRK_VOL_MAX + pan1)) / ATRK_VOL_MAX;
	} else {
		vol1_l = (vol1 * (ATRK_VOL_MAX - pan1)) / ATRK_VOL_MAX;
		vol1_r = vol1;
	}

	if (pan2 == 0) {
		vol2_l = vol2_r = vol2;
	} else if (pan2 < 0) {
		vol2_l = vol2;
		vol2_r = (vol2 * (ATRK_VOL_MAX + pan2)) / ATRK_VOL_MAX;
	} else {
		vol2_l = (vol2 * (ATRK_VOL_MAX - pan2)) / ATRK_VOL_MAX;
		vol2_r = vol2;
	}

	while (bytes > 3) {
		/* LEFT SAMPLE */
		d1 = src1 ? (*src1++) : 0;
		d2 = src2 ? (*src2++) : 0;
		bytes -= 2;

		d1 = (d1 * vol1_l) / ATRK_VOL_MAX;
		d2 = (d2 * vol2_l) / ATRK_VOL_MAX;
		d1 = d1 + 32768;	/* s16 sample to u16 */
		d2 = d2 + 32768;

		if (d1 < 32768 && d2 < 32768) {
			d3 = (d1 * d2) / 32768;
		} else {
			d3 = (2 * (d1 + d2)) - ((d1 * d2) / 32768) - 65536;
		}
		*dst++ = (d3 - 32768);	/* mixed u16 back to s16 and write */

		/* RIGHT SAMPLE */
		d1 = src1 ? (*src1++) : 0;
		d2 = src2 ? (*src2++) : 0;
		bytes -= 2;

		d1 = (d1 * vol1_r) / ATRK_VOL_MAX;
		d2 = (d2 * vol2_r) / ATRK_VOL_MAX;
		d1 = d1 + 32768;		/* s16 sample to u16 */
		d2 = d2 + 32768;

		if (d1 < 32768 && d2 < 32768) {
			d3 = (d1 * d2) / 32768;
		} else {
			d3 = (2 * (d1 + d2)) - ((d1 * d2) / 32768) - 65536;
		}

		*dst++ = (d3 - 32768);	/* mixed u16 back to s16 and write */
	}
}

static int aud_mix_tracks(struct sanctx *ctx)
{
	struct sanmsa *msa = ctx->msa;
	int16_t *trkobuf = (int16_t *)msa->audrsb1;
	int active1, active2, mixable, voice;
	uint32_t minlen1, dstlen, dff;
	uint8_t *dstptr, *aptr;
	struct sanatrk *atrk;

	dstlen = 0;
	dstptr = aptr = ctx->adstbuf1;
	memset(dstptr, 0, msa->audminframes * 4);

	dff = msa->audminframes;	/* amount of target samples to generate */

	active1 = atrk_count_active(msa, &voice);
	while ((active1 != 0) && (dff != 0)) {
		atrk_reset_mixed(msa);
		mixable = atrk_count_mixable(msa, &minlen1);
		if (!mixable)
			break;

		if (dff && (minlen1 == -1)) {
			/* hmm underrun */
			break;
		}

		if (minlen1 > dff)
			minlen1 = dff;

		while (NULL != (atrk = atrk_get_next_mixable(msa))) {
			const int m = atrk->pflags & SAUD_FLAG_TRK_MASK;
			int vol = 0, pan;
			if (m == 0) {
				vol = (atrk->vol * msa->sou_vol_sfx) >> 7;
			} else if (m == SAUD_FLAG_TRK_VOICE) {
				vol = (atrk->vol * msa->sou_vol_voice) >> 7;
			} else if (m == SAUD_FLAG_TRK_MUSIC) {
				vol = (atrk->vol * msa->sou_vol_music) >> 7;
			}
			vol = (vol * msa->sou_vol_global) >> 7;
			if (m == SAUD_FLAG_TRK_MUSIC)
				vol = ((vol * msa->sou_vol_damp) >> 8) & 0xff;

			pan = (atrk->flags & ATRK_1CH) ? atrk->pan : 0;
			atrk_convert_resample(atrk, trkobuf, minlen1);
			aud_mixs16(dstptr, (uint8_t *)trkobuf, dstptr,
				   minlen1 * 4, vol, pan, ATRK_VOL_MAX, 0);
			atrk->flags |= ATRK_MIXED;
		}
		dstlen += minlen1 * 4;
		dstptr += dstlen;
		dff -= minlen1;
		if (dff) {
			active2 = atrk_count_active(msa, NULL);
			if (active2 < active1) {
				/* the hopefully short track was done and freed,
				 * try again to get the missing rest from the
				 * other tracks.
				 */
				active1 = active2;
				continue;
			}
			/* see if any of the tracks had a reset of their play
			 * position/length, indicated by the ATRK_REMIX flag.
			 * Then we can clear its MIXED flag and reconsider it
			 * for further processing.
			 */
			for (int i = 0; i < msa->numtrk; i++) {
				atrk = &(msa->atrk[i]);
				if (atrk->flags & ATRK_REMIX)
					atrk->flags &= ~ATRK_MIXED;
				atrk->flags &= ~ATRK_REMIX;
			}
			/* final check: newly mixable tracks with data available */
			mixable = atrk_count_mixable(msa, &minlen1);
			if ((mixable > 0) && (minlen1 != -1)) {
				/* ok, more data just appeared, try again with
				 * all active tracks. */
				continue;
			}
			/* one track is experiencing an underrun, exit. */
			break;
		}
	}
	if (dstlen)
		ctx->io->queue_audio(ctx->io->userctx, aptr, dstlen);

	if (voice) {
		if (msa->sou_vol_damp != msa->sou_damp_min)
			msa->sou_vol_damp -= msa->audminframes / msa->sou_damp_dip_rate;
		if (msa->sou_vol_damp < msa->sou_damp_min)
			msa->sou_vol_damp = msa->sou_damp_min;
	} else {
		if (msa->sou_vol_damp != msa->sou_damp_max)
			msa->sou_vol_damp += msa->audminframes / msa->sou_damp_rise_rate;
		if (msa->sou_vol_damp > msa->sou_damp_max)
			msa->sou_vol_damp = msa->sou_damp_max;
	}
	atrk_reset_mixed(msa);
	return (dstlen != 0);
}

static void iact_audio_imuse(struct sanctx *ctx, uint32_t size, uint8_t *src,
			      uint16_t trkid, uint16_t uid)
{
	uint32_t cid, csz, mapsz;
	uint16_t rate, bits, chnl, vol;
	struct sanatrk *atrk;

	vol = ATRK_VOL_MAX;
	if (uid == 1)
		trkid += 100;
	else if (uid == 2)
		trkid += 200;
	else if (uid == 3)
		trkid += 300;
	else if ((uid >= 100) && (uid <= 163)) {
		trkid += 400;
		vol = uid * 2 - 200;
	} else if ((uid >= 200) && (uid <= 263)) {
		trkid += 500;
		vol = uid * 2 - 400;
	} else if ((uid >= 300) && (uid <= 363)) {
		trkid += 600;
		vol = uid * 2 - 600;
	}

	atrk = atrk_find_trkid(ctx->msa, trkid, 0);
	if (!atrk)
		return;
	atrk->trkid = trkid;

	if (vol > ATRK_VOL_MAX)
		vol = ATRK_VOL_MAX;
	rate = ctx->msa->samplerate;
	bits = 12;
	chnl = 1;

	/*
	 * read header of new track. NOTE: subchunks aren't 16bit aligned!
	 */
	if (0 == (atrk->flags & ATRK_INUSE)) {
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
			}

			src += csz;
			size -= csz;
			mapsz -= csz;
		}

		/* now there should be "DATA" with the TOTAL len of sound of the whole track */
		if (size < 8)
			return;
		cid = le32_to_cpu(ua32(src + 0));
		csz = be32_to_cpu(ua32(src + 4));
		src += 8;
		size -= 8;
		if (cid != DATA)
			return;

		atrk->flags |= ATRK_INUSE | ATRK_BLOCKED;	/* active track */
		atrk->dataleft = csz;
		atrk_set_srcfmt(atrk, rate, bits, chnl, vol, 0);
		atrk_set_default_strk(atrk, csz);
		atrk_process_strk(atrk);
	}

	atrk_read_pcmsrc(atrk, size, src);
	atrk->dataleft -= size;
	if (atrk->dataleft <= 0) {
		atrk->flags &= ~ATRK_BLOCKED;
		atrk->dataleft = 0;
	}
	if (atrk->dstfavail >= ctx->msa->audminframes)
		atrk->flags &= ~ATRK_BLOCKED;
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
				if (ctx->rt.iactpos + size > SZ_IACT)
					return;
				memcpy(ib + ctx->rt.iactpos, src, size);
				ctx->rt.iactpos += size;
				size = 0;
			} else {
				if (ctx->rt.iactpos + len > SZ_IACT)
					return;
				memcpy(ib + ctx->rt.iactpos, src, len);
				dst = (int16_t *)ctx->adstbuf1;
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
				ctx->io->queue_audio(ctx->io->userctx, ctx->adstbuf1, 4096);
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

static void handle_IACT(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint16_t p[7];
	int i;

	for (i = 0; i < 7; i++)
		p[i] = le16_to_cpu(*(uint16_t*)(src + (i<<1)));

	if (p[0] == 8 && p[1] == 46) {
		if (ctx->io->flags & SANDEC_FLAG_NO_AUDIO)
			return;

		if (p[3] == 0) {
			/* subchunkless scaled IACT audio codec47/48 videos */
			iact_audio_scaled(ctx, size - 18, src + 18);
		} else {
			/* imuse-type */
			iact_audio_imuse(ctx, size - 18, src + 18, p[4], p[3]);
		}
	}
}

static void handle_SAUD(struct sanctx *ctx, uint32_t size, uint8_t *src,
			const uint32_t tid, const uint8_t vol, const int8_t pan,
			const uint32_t maxidx, const uint16_t pflags)
{
	uint32_t cid, csz;
	uint16_t rate;
	struct sanatrk *atrk;

	atrk = atrk_find_trkid(ctx->msa, tid, 0);
	if (!atrk)
		return;

	/* RA1 does this */
	if (atrk->flags != 0) {
		atrk_reset(atrk);
	}

	atrk->trkid = tid;
	atrk->maxidx = maxidx;
	atrk->pflags = pflags;
	rate = ctx->msa->samplerate;
	while (size > 7) {
		cid = le32_to_cpu(ua32(src + 0));
		csz = be32_to_cpu(ua32(src + 4));
		src += 8;
		size -= 8;
		if (cid == STRK) {
			if (csz < ATRK_MAX_STRK_SIZE) {
				memcpy(atrk->strk, src, csz);
				atrk->strksz = csz;
			}

		} else if (cid == SDAT) {
			atrk->flags |= ATRK_INUSE | ATRK_BLOCKED;
			atrk->dataleft = csz;
			break;
		}
		src += csz;
		size -= csz;
	}
	atrk_set_srcfmt(atrk, rate, 8, 1, vol, pan);
	atrk_read_pcmsrc(atrk, size, src);
	atrk->dataleft -= size;
	if ((atrk->dataleft <= 0) || (maxidx < 2)) {
		atrk->flags &= ~ATRK_BLOCKED;
	}
	if (atrk->dstfavail >= ctx->msa->audminframes)
		atrk->flags &= ~ATRK_BLOCKED;
	atrk_process_strk(atrk);
}

static void handle_PSAD(struct sanctx *ctx, uint32_t size, uint8_t *src, uint8_t v1flag)
{
	uint32_t t1, tid, idx, vol, pan, mid, flg;
	struct sanatrk *atrk;

	if (ctx->io->flags & SANDEC_FLAG_NO_AUDIO)
		return;

	/* dig.exe 4332f */
	if ((src[0] == 0) && (src[1] == 0) && (src[4] == 0) &&
	    (src[5] == 0) && (src[8] == 0) && (src[9] == 0)) {
		/* PSADv1, as in Rebel Assault 1 / ANIMv1 */
		tid = be32_to_cpu(ua32(src + 0));
		idx = be32_to_cpu(ua32(src + 4));
		mid = be32_to_cpu(ua32(src + 8));
		flg = v1flag;
		vol = ATRK_VOL_MAX;	/* maximum */
		pan = 0;		/* centered */
		src += 12;
		size -= 12;
	} else {
		/* PSADv2, as in Rebel Assault 2+ / ANIMv2 */
		tid = le16_to_cpu(ua16(src + 0));
		idx = le16_to_cpu(ua16(src + 2));
		mid = le16_to_cpu(ua16(src + 4));
		flg = le16_to_cpu(ua16(src + 6));
		vol = src[8];

		if (vol > ATRK_VOL_MAX)
			vol = ATRK_VOL_MAX;

		/* RA2 mono sound supposed to be played on both channels.
		 * used for the music mostly. Set pan to zero since the source
		 * is expanded to both channels if necessary when read.
		 */
		if (src[9] == 0x80)
			pan = 0;
		else
			pan = (int8_t)src[9];

		src += 10;
		size -= 10;
	}

	if (idx == 0) {
		/* should be a "SAUD____" here; the "size" is the size of the
		 * _complete_ SAUD in all the PSAD chunks.
		 */
		t1 = le32_to_cpu(ua32(src + 0));
		if (t1 == SAUD) {
			handle_SAUD(ctx, size - 8, src + 8, tid, vol, pan, mid, flg);
		}
	} else {
		/* handle_SAUD should have allocated it.  RA1 however
		 * sometimes repeats the last index of a tid a few times.
		 */
		atrk = atrk_find_trkid(ctx->msa, tid, 1);
		if (!atrk) {
			return;
		}
		/* sou_engine_start_stream() does this in all engines */
		if (((atrk->curridx + 1) != idx) || (atrk->maxidx != mid))
			return;

		atrk->curridx = idx;
		atrk->pan = pan;
		atrk->vol = vol;
		if (size > atrk->dataleft)
			size = atrk->dataleft;
		atrk_read_pcmsrc(atrk, size, src);
		atrk->dataleft -= size;
		if ((atrk->dataleft < 1) ||
		    (atrk->dstfavail >= ctx->msa->audminframes)) {
			atrk->flags &= ~ATRK_BLOCKED;
		}
	}
}

static int handle_IMA4(struct sanctx *ctx, uint32_t size, uint8_t *src,
			uint32_t samples, int ch)
{
	int16_t *dst = (int16_t *)ctx->adstbuf1;
	int i, j, nibsel, tblidx, adpcm_step, dat, delt;
	uint8_t in, nib;

	dat = 0;
	nibsel = 0;
	in = 0;

	if (size < 3)
		return 89;
	dat = (int16_t)le16_to_cpu(ua16(src));
	src += 2;
	tblidx = *src++;
	size -= 3;
	i = 0;
	/* this is IMA ADPCM QT */
	while ((size > 0) && (i < samples)) {
		if (nibsel == 0) {
			if (size < 1)
				break;
			in = *src++;
			nib = in >> 4;
			size--;
		} else {
			nib = in & 0x0f;
		}
		nibsel = !nibsel;

		if (tblidx < 0)
			tblidx = 0;
		else if (tblidx > 88)
			tblidx = 88;
		adpcm_step = adpcm_step_table[tblidx];
		tblidx += ima4_tab[nib];
		delt = adpcm_step >> 3;
		if (nib & 4)
			delt += adpcm_step;
		if (nib & 2)
			delt += (adpcm_step >> 1);
		if (nib & 1)
			delt += (adpcm_step >> 2);
		if (nib & 8)
			dat -= delt;
		else
			dat += delt;

		if (dat < -0x8000)
			dat = -0x8000;
		else if (dat > 0x7fff)
			dat = 0x7fff;

		for (j = 0; j < ch; j++)
			*dst++ = dat;

		i++;
	}
	ctx->io->queue_audio(ctx->io->userctx, ctx->adstbuf1, i * 2 * ch);
	return 0;
}

static int handle_VIMA(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	int i, j, v1, data, ch, inbits, numbits, bitsize;
	int  hibit, lobits, tblidx, idx2, delt;
	int16_t startdata[2], *dst;
	uint8_t startpos[2];
	uint32_t samples, sig;

	if (size < 16)
		return 86;

	samples = be32_to_cpu(ua32(src));
	src += 4;
	size -= 4;
	if ((int32_t)samples < 0) {
		samples = be32_to_cpu(ua32(src + 4));
		src += 8;
		size -= 8;
	}

	ch = 1;
	startpos[0] = *src++;
	size--;
	if (startpos[0] & 0x80) {
		startpos[0] = ~startpos[0];
		ch = 2;
	}

	startdata[0] = be16_to_cpu(ua16(src));
	src += 2;
	size -= 2 ;
	if (ch > 1) {
		startpos[1] = *src++;
		size--;
		startdata[1] = be16_to_cpu(ua16(src));
		src += 2;
		size -= 2;
	}

	if ((samples * 2 * ch) > SZ_ADSTBUF)
		return 87;

	/* here could be IMA4 */
	sig = be32_to_cpu(ua32(src));
	if (sig == IMA4) {
		src += 4;
		size -= 4;
		handle_IMA4(ctx, size, src, samples, ch);
		return 0;
	}

	inbits = be16_to_cpu(ua16(src));
	src += 2;
	size -= 2;
	numbits = 0;

	memset(ctx->adstbuf1, 0, samples * 2 * ch);
	for (i = 0; i < ch; i++) {
		dst = (int16_t *)ctx->adstbuf1 + i;
		tblidx = startpos[i];
		data = startdata[i];

		for (j = 0; j < samples; j++) {
			bitsize = vima_size_table[tblidx];
			numbits += bitsize;
			hibit = 1 << (bitsize - 1);
			lobits = hibit - 1;
			v1 = (inbits >> (16 - numbits)) & (hibit | lobits);

			if (numbits > 7) {
				if (!size)
					break;
				inbits = ((inbits & 0xff) << 8) | *src++;
				numbits -= 8;
				size--;
			}

			if (v1 & hibit)
				v1 ^= hibit;
			else
				hibit = 0;

			if (v1 == lobits) {
				data = ((int16_t)(inbits << numbits) & 0xffffff00);
				inbits = ((inbits & 0xff) << 8) | *src++;
				data |= ((inbits >> (8 - numbits)) & 0xff);
				inbits = ((inbits & 0xff) << 8) | *src++;
				size -= 2;
			} else {
				idx2 = (v1 << (7 - bitsize)) | (tblidx << 6);
				delt = ctx->vima_pred_tbl[idx2];

				if (v1)
					delt += (adpcm_step_table[tblidx] >> (bitsize - 1));
				if (hibit)
					delt = -delt;

				data += delt;
				if (data < -0x8000)
					data = -0x8000;
				else if (data > 0x7fff)
					data = 0x7fff;
			}

			*((uint16_t *)dst) = data;
			dst += ch;

			tblidx += vima_itbls[bitsize - 2][v1];
			if (tblidx < 0)
				tblidx = 0;
			else if (tblidx > (ADPCM_STEP_COUNT - 1))
				tblidx = ADPCM_STEP_COUNT - 1;

		}
		if (!size)
			break;
	}

	ctx->io->queue_audio(ctx->io->userctx, ctx->adstbuf1, samples * 2 * ch);
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
	/* STOR usually caches the FOBJ raw data in the aux buffer.
	 * In RA1 however, there's the option to decode the image immediately,
	 * and store that instead with a standard FOBJ codec header with codec20
	 * and 320x200 size (I guess for perf reasons in 1993).
	 * This is indicated by STOR data 0 being 3.
	 */
	ctx->rt.to_store = (src[0] == 3 ? 2 : 1);
}

static int handle_FTCH(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint8_t *vb = ctx->rt.buf3;
	int16_t xoff, yoff;
	uint32_t sz;
	int ret;

	if (size == 6) {
		xoff = le16_to_cpu(*(int16_t *)(src + 2));
		yoff = le16_to_cpu(*(int16_t *)(src + 4));
	} else if (size == 12) {
		xoff = (int16_t)be32_to_cpu(ua32(src + 4));
		yoff = (int16_t)be32_to_cpu(ua32(src + 8));
	} else
		return 0;

	ret = 0;
	sz = *(uint32_t *)(vb + 0);
	if (sz > 0 && sz <= ctx->rt.fbsize) {
		ret = handle_FOBJ(ctx, sz, vb + 4, xoff, yoff);
	}
	ctx->rt.can_ipol = 0;
	if (ret == 0)
		ctx->rt.have_frame = 1;
	return ret;
}

/* allocate memory for a full FRME */
static int allocfrme(struct sanctx *ctx, uint32_t sz)
{
	if (sz > FRME_MAX_SIZE)	/* cap at 4MB */
		return 99;
	sz = (sz + 4095) & ~4095U;
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
		return 14;

	ret = 0;
	while ((size > 7) && (ret == 0)) {

		/* some blocks like IACT have odd size, and RA1 L2PLAY.ANM
		 * has a few unaligned (not at 2 byte boundary) FOBJs.
		 * This is how the smush game engine deals with that.
		 */
		if (((uintptr_t)src & 1) && (*src == 0)) {
			src++;
			size--;
		}

		cid = le32_to_cpu(ua32(src + 0));
		csz = be32_to_cpu(ua32(src + 4));

		src += 8;
		size -= 8;

		if (csz > size)
			return 15;

		if (rt->version > 2) {
			switch (cid) {
			case WAVE: ret = handle_VIMA(ctx, csz, src); break;
			case BL16: handle_BL16(ctx, csz, src); break;
			default:   ret = 0;
			}
		} else {	
			switch (cid) {
			case NPAL: handle_NPAL(ctx, csz, src); break;
			case FOBJ: ret = handle_FOBJ(ctx, csz, src, 0, 0); break;
			case IACT: handle_IACT(ctx, csz, src); break;
			case TRES: handle_TRES(ctx, csz, src); break;
			case STOR: handle_STOR(ctx, csz, src); break;
			case FTCH: ret = handle_FTCH(ctx, csz, src); break;
			case XPAL: handle_XPAL(ctx, csz, src); break;
			case PVOC: handle_PSAD(ctx, csz, src, SAUD_FLAG_TRK_VOICE); break;
			case PSD2: handle_PSAD(ctx, csz, src, SAUD_FLAG_TRK_SFX);   break;
			case PSAD: handle_PSAD(ctx, csz, src, SAUD_FLAG_TRK_MUSIC); break;
			default:   ret = 0;		/* unknown chunk, ignore */
			}
		}
		src += csz;
		size -= csz;
	}

	/* OK case: all usable bytes of the FRME read, no errors */
	if (ret == 0) {
		if (ctx->rt.have_frame) {
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
				memcpy(rt->buf4, rt->vbuf, rt->bufw * rt->bufh * 1);
				ctx->io->queue_video(ctx->io->userctx, rt->buf5,
					     rt->pitch * rt->bufh,
					     rt->bufw, rt->bufh, rt->pitch, rt->palette,
					     rt->subid, rt->framedur / 2);
			} else {
				ctx->io->queue_video(ctx->io->userctx, rt->vbuf,
					     rt->pitch * rt->bufh * 1,
					     rt->bufw, rt->bufh, rt->pitch, rt->palette,
					     rt->subid, rt->framedur);
				/* save frame as possible interpolation source */
				if (rt->have_itable)
					memcpy(rt->buf4, rt->vbuf, rt->bufw * rt->bufh * 1);
			}
		}

		/* mix multi-track audio and queue it up */
		if (!(ctx->io->flags & SANDEC_FLAG_NO_AUDIO) && (ctx->msa))
			aud_mix_tracks(ctx);

		rt->currframe++;
		rt->subid = 0;
		rt->have_frame = 0;
	}

	return ret;
}

static void vima_init(struct sanctx *ctx)
{
	int i, j, k, l, m, n;

	for (i = 0; i < 64; i++) {
		for (j = 0, k = i; j < ADPCM_STEP_COUNT; j++, k += 64) {
			n = 0;
			l = adpcm_step_table[j];
			for (m = 32; m != 0; m >>= 1) {
				if (i & m)
					n += l;
				l >>= 1;
			}
			ctx->vima_pred_tbl[k] = n;
		}
	}
}

static void sandec_free_memories(struct sanctx *ctx)
{
	if (ctx->msa)
		free(ctx->msa);
	/* delete existing FRME buffer */
	if (ctx->rt.fcache)
		free(ctx->rt.fcache);
	/* delete work + video buffers, iactbuf is entry point */
	if (ctx->rt.membase)
		free(ctx->rt.membase);
	memset(&ctx->rt, 0, sizeof(struct sanrt));
}

static int sandec_alloc_msa(struct sanmsa **msa_out, uint8_t num_trks,
			    uint32_t audminframes)
{
	struct sanmsa *msa;
	uint32_t mem, grsb;
	uint8_t *m;
	int i;

	/* ANM audio track buffers, resample structures */
	mem = sizeof(struct sanmsa);
	/* channels buffer memories */
	mem += num_trks * ATRK_DATSZ;
	
	/* resampling/mixing buffer */
	grsb = 4 * audminframes;
	mem += grsb;

	m = malloc(mem);
	if (!m)
		return 1;
	memset(m, 0, mem);

	msa = (struct sanmsa *)m;
	m += sizeof(struct sanmsa);

	msa->numtrk = num_trks;
	msa->audminframes = audminframes;

	/* PSAD/iMUS audio track buffers */;
	for (i = 0; i < msa->numtrk; i++) {
		msa->atrk[i].msa = msa;
		msa->atrk[i].data = m;
		m += ATRK_DATSZ;
	}
	msa->audrsb1 = (uint8_t *)m;	m += grsb;

	atrk_init_volumes(msa);
	*msa_out = msa;
	return 0;
}

static int sandec_alloc_vidmem(struct sanctx *ctx, const uint16_t maxx,
			        const uint16_t maxy, const int sanm)
{
	uint32_t vmem, cmem, mem, gb;
	struct sanrt *rt = &ctx->rt;
	uint8_t *m;

	mem = 0;
	vmem = 0;

	/* codec buffers: 43 lines (max. c47 mv) top + bottom as guard bands
	 * for stray motion vectors.
	 */
	cmem = (sanm ? 2 : 1) * maxx * (maxy + 88);
	cmem = (cmem + 63) & ~63;		/* 4k align */
	gb = (sanm ? 2 : 1) * 43 * maxx;	/* guard band size */
	gb = (gb + 63) & ~63;			/* align */

	/* we need 3 private buffers for codec37/47/48 and bl16 */
	mem += (cmem * 3);

	/* BL16 uses its own active buffer to display the image, since
	 * there are no other codecs which can modify its image.
	 * For ANM however we need: 1 image buffer for codecs1-33,
	 * STOR buffer and 2 image buffers to interpolate frames,
	 * plus space for the PSAD/iMUS audio tracks.
	 */
	if (!sanm) {
		/* Palettes/Interpolation Table/IACT-scaled-audio buf */
		mem += SZ_ANMBUFS;
		/* ANM aux buffers: fbuf, buf3/4/5 */
		vmem = maxx * maxy;
		vmem = (vmem + 63) & ~63;		/* align */
		mem += vmem * 4;
		/* STOR buffer c20 FOBJ header + data size */
		mem += 32;
		mem = (mem + 4095) & ~4095;
	}

	/* allocate memory for work buffers */
	m = (uint8_t *)malloc(mem);
	if (!m)
		return 1;
	rt->membase = m;
	memset(m, 0, mem);

	/* align start to cacheline size */
	m = (uint8_t *)((((uintptr_t)m) + 63) & ~63);

	if (!sanm) {
		/* ANIM/ANM misc buffers */
		rt->iactbuf = (uint8_t *)m;	m += SZ_IACT;
		rt->palette = (uint32_t *)m; 	m += SZ_PAL;
		rt->deltapal = (int16_t *)m;	m += SZ_DELTAPAL;
		rt->shiftpal = (int16_t *)m;	m += SZ_SHIFTPAL;
		rt->c47ipoltbl = (uint8_t *)m;	m += SZ_C47IPTBL;

		/* set up video buffers for ANM */
		m = (uint8_t *)((((uintptr_t)m) + 63) & ~63);
		rt->fbuf = m;			/* front buffer 		*/
		rt->buf0 = rt->fbuf + vmem;	/* codec37/47/48/bl16 main buf	*/
		rt->buf1 = rt->buf0 + cmem;	/* delta buf 2 (c47/bl16),	*/
		rt->buf2 = rt->buf1 + cmem;	/* delta buf 1 (c37/47/48/bl16)	*/
		rt->buf4 = rt->buf2 + vmem;	/* interpolated frame buffer	*/
		rt->buf5 = rt->buf4 + vmem;	/* interpolation last frame buf */
		rt->buf3 = rt->buf5 + cmem;	/* STOR buffer			*/
	} else {
		/* set up buffers for SNM: 3 video buffers for BL16 */
		rt->fbuf = NULL;
		rt->buf0 = m;			/* bl16 main 			*/
		rt->buf1 = rt->buf0 + cmem;	/* bl16 delta buffer 1		*/
		rt->buf2 = rt->buf1 + cmem;	/* bl16 delta buffer 2		*/
	}
	/* offset the starts of the codec buffers with guard bands, so
	 * each one has 43 lines before start and after end to account
	 * for stray/invalid motion vectors.
	 */
	rt->buf0 += gb;
	rt->buf1 += gb;
	rt->buf2 += gb;

	return 0;
}

static int handle_AHDR(struct sanctx *ctx, uint32_t size)
{
	struct sanrt *rt = &ctx->rt;
	uint32_t maxframe, audminframes, srate;
	uint8_t *ahbuf, fps;

	if (size > 794)
		return 14;
	if (size < 774)
		return 15;

	ahbuf = (uint8_t *)malloc(size);
	if (!ahbuf)
		return 11;
	if (read_source(ctx, ahbuf, size)) {
		free(ahbuf);
		return 12;
	}

	rt->version = le16_to_cpu(*(uint16_t *)(ahbuf + 0));
	rt->FRMEcnt = le16_to_cpu(*(uint16_t *)(ahbuf + 2));

	if (0 != sandec_alloc_vidmem(ctx, FOBJ_MAXX, FOBJ_MAXY, 0)) {
		free(ahbuf);
		return 4;
	}

	read_palette(ctx, ahbuf + 6);	/* 768 bytes */

	if (rt->version > 1) {
		rt->framedur  =  le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 0));
		fps = rt->framedur;
		maxframe =       le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 4));
		srate	       = le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 8));

		/* "maxframe" indicates the maximum size of one FRME object
		 * including chunk ID and chunk size in the stream (usually the first)
		 * plus 1 byte.
		 */
		if ((maxframe > 9) && (maxframe <= FRME_MAX_SIZE)) {
			if (allocfrme(ctx, maxframe)) {
				free(ahbuf);
				return 13;
			}
		}
	} else {
		fps = 15;			/* ANIMv1 default */
		srate = 11025;			/* ANIMv1 default */
		rt->frmebufsz = 0;
	}
	free(ahbuf);

	if (!fps)
		fps = 15;
	rt->framedur = 1000000 / fps;	/* frame duration in microseconds */

	/* minimum number of samples to generate when resampling a stream to the
	 * output rate to supply enough data for the duration of a single frame.
	 */
	audminframes = ((22050 / fps) + 1) & ~1U;

	/* for Full Throttle: the incoming audio data rate is not not enough
	 * to sustain click-free playback at the requested 10fps.  It starts
	 * to work at 10.3 fps; use 10.5fps since it divides the incoming sample-
	 * rate without a remainder and the 95ms frame time keeps good lip sync
	 * in all videos.
	 */
	if (fps < 11) {
		rt->framedur = 10000000 / 105;
		audminframes = (((22050 * 10) / 105) + 1) & ~1U;
	}

	if (sandec_alloc_msa(&ctx->msa, ATRK_MAX, audminframes))
		return 14;
	ctx->msa->samplerate = srate;
	
	return 0;
}

static int handle_SHDR(struct sanctx *ctx, uint32_t csz)
{
	struct sanrt *rt = &ctx->rt;
	uint16_t maxx, maxy, t16;
	uint32_t c[2], sz, srate, achans;
	uint8_t *src, *sb;
	int ret;

	if (csz > 4096)
		return 57;

	/* even the odds */
	if (csz & 1)
		csz += 1;

	src = malloc(csz);
	if (!src)
		return 50;
	if (read_source(ctx, src, csz)) {
		free(src);
		return 51;
	}

	rt->version = 3;
	rt->FRMEcnt = le32_to_cpu(ua32(src + 2));
	rt->bufw = le16_to_cpu(*(uint16_t *)(src + 8));
	rt->bufh = le16_to_cpu(*(uint16_t *)(src + 10));
	rt->framedur = le32_to_cpu(ua32(src + 14));
	maxx = rt->bufw;
	maxy = rt->bufh;
	free(src);

	/* there's now >1kB of data left, no idea what it's for.
	 * at the end there should be FLHD
	 */
	if (read_source(ctx, c, 8)) {
		return 52;
	}
	if (c[0] != le32_to_cpu(FLHD)) {
		return 53;
	}
	sz = be32_to_cpu(c[1]);
	if (sz > 4096)
		return 58;
	src = malloc(sz);
	if (!src)
		return 54;
	if (read_source(ctx, src, sz)) {
		free(src);
		return 55;
	}

	/* FLHD has again a subchunks for video and audio format.
	 *  Audio is most interesting, but parse video as well in
	 *  case the SANM header and the BL16 chunk(s) disagree.
	 */
	ret = 0;
	sb = src;
	while ((sz > 7) && (ret == 0)) {
		c[0] = le32_to_cpu(ua32(src + 0));
		c[1] = be32_to_cpu(ua32(src + 4));
		src += 8;
		sz -= 8;
		if (c[1] > sz)
			break;
		switch (c[0]) {
		case BL16:
			if (c[1] != 8)
				break;
			t16 = le16_to_cpu(*(uint16_t *)(src + 2));
			if (t16 > maxx)
				maxx = t16;
			t16 = le16_to_cpu(*(uint16_t *)(src + 4));
			if (t16 > maxy)
				maxy = t16;
			break;
		case WAVE:
			srate  = le32_to_cpu(ua32(src + 0));
			achans = le32_to_cpu(ua32(src + 4));
			if (srate != 22050)
				ret = 57;
			if ((achans < 1) || (achans > 2))
				ret = 58;
			c[1] = 12;
			break;
		default:
			ret = 56;
		}
		if (c[1] & 1)
			c[1]++;

		sz -= c[1];
		src += c[1];
	}
	free(sb);

	if ((maxx > 1024) || (maxy > 768))
		return 59;
	if (0 != sandec_alloc_vidmem(ctx, maxx, maxy, 1))
		return 4;

	rt->bufw = maxx;
	rt->bufh = maxy;
	rt->pitch = 2 * rt->bufw;

	vima_init(ctx);

	return ret;
}

/******************************************************************************/
/* public interface */

int sandec_decode_next_frame(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	uint32_t c[2];
	int ret, b;

	if (!ctx)
		return 1;
	/* in case of previous error, don't continue, just return it again */
	if (ctx->errdone)
		return ctx->errdone;

	/* interpolated frame: was queued first, now queue the decoded one */
	if (ctx->rt.have_ipframe) {
		struct sanrt *rt = &ctx->rt;
		rt->have_ipframe = 0;
		ctx->io->queue_video(ctx->io->userctx, rt->vbuf, rt->pitch * rt->bufh,
				     rt->bufw, rt->bufh, rt->pitch, rt->palette,
				     rt->subid, rt->framedur / 2);
		return SANDEC_OK;
	}

again:
	ret = read_source(ctx, c, 8);
	if (ret) {
		if (ctx->rt.currframe >= ctx->rt.FRMEcnt) {
			ret = SANDEC_DONE;	/* seems we reached file end */
			if (ctx->msa) {
				atrk_finish_all(ctx->msa);
				b = 1;
				while (b && atrk_count_active(ctx->msa, NULL) &&
					!(ctx->io->flags & SANDEC_FLAG_NO_AUDIO)) {
					b = aud_mix_tracks(ctx);
				}
			}
		}
		goto out;
	}

	c[1] = be32_to_cpu(c[1]);
	if (c[0] == FRME) {
		/* default case */
		ret = handle_FRME(ctx, c[1]);

	} else if (c[0] == ANNO) {
		/* some annotation, found esp. in Grim Fandango files. just skip it */
		uint8_t buf[128], rs;
		while (c[1] > 0) {
			rs = c[1] >= 128 ? 128 : c[1];
			ret = read_source(ctx, buf, rs);
			if (ret) {
				ret = 11;
				goto out;
			}
			c[1] -= rs;
		}
		goto again;

	} else if (atrk_count_active(ctx->msa, NULL) && !(ctx->io->flags & SANDEC_FLAG_NO_AUDIO)) {
		aud_mix_tracks(ctx);

	} else {
		ret = 10;
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
		return 2;
	memset(ctx, 0, sizeof(struct sanctx));
	ctx->adstbuf1 = (uint8_t *)malloc(SZ_ADSTBUF);
	if (!ctx->adstbuf1) {
		free(ctx);
		return 3;
	}

	/* set to error state initially until a valid file has been opened */
	ctx->errdone = 44;

	c47_make_glyphs(&ctx->c47_glyph4x4[0][0], c47_glyph4_x, c47_glyph4_y, 4);
	c47_make_glyphs(&ctx->c47_glyph8x8[0][0], c47_glyph8_x, c47_glyph8_y, 8);
	*ctxout = ctx;

	return 0;
}

int sandec_open(void *sanctx, struct sanio *io)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	uint32_t c[2];
	int ret;

	if (!io || !sanctx) {
		ret = 3;
		goto out;
	}
	ctx->io = io;

	sandec_free_memories(ctx);

	/* impossible value in case a FOBJ with param1 == 0 comes first */
	ctx->c4tblparam = 0xffff;

	/* files can either start with "ANIM____AHDR___", "SANM____SHDR____" or
	 * "SAUD____".
	 */
	ret = read_source(ctx, &c[0], 4 * 2);
	if (ret) {
		ret = 5;
		goto out;
	}
	if ((c[0] == ANIM) || (c[0] == SANM)) {
		ret = read_source(ctx, &c[0], 4 * 2);
		if (c[0] == AHDR) {
			ret = handle_AHDR(ctx, be32_to_cpu(c[1]));
		} else if (c[0] == SHDR) {
			ret = handle_SHDR(ctx, be32_to_cpu(c[1]));
		} else {
			ret = 7;
		}

	} else if (c[0] == SAUD) {
		uint8_t *dat;
		uint32_t csz = be32_to_cpu(c[1]);
		ret = 8;
		if ((csz < 8) || (csz > (1 << 20))) {
			goto out;
		} else {
			if (sandec_alloc_msa(&ctx->msa, 1, 22050))
				goto out;
			dat = malloc(csz);
			if (!dat)
				goto out;
			if (read_source(ctx, dat, csz)) {
				free(dat);
				goto out;
			}
			ctx->msa->samplerate = 11025;
			handle_SAUD(ctx, csz, dat, 1, ATRK_VOL_MAX, 0, 1, 0);
			free(dat);
			ret = atrk_count_active(ctx->msa, NULL) > 0 ? 0 : 8;
		}

	} else {
		ret = 9;
	}

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
