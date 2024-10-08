/*
 * SAN ANIM file decoder for Outlaws ".SAN" video files.
 * Outlaws SAN files use SMUSH codecs 1+47 and IACT 22.05kHz 16-bit stereo Audio.
 *
 * Written in 2024 by Manuel Lauss <manuel.lauss@gmail.com>
 *
 * Codec algorithms (Video, Audio, Palette) liberally taken from FFmpeg and
 * ScummVM:
 * https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/HEAD:/libavcodec/sanm.c
 * https://github.com/scummvm/scummvm/blob/master/engines/scumm/smush/smush_player.cpp
 */

#include <byteswap.h>
#include <stdlib.h>
#include <string.h>
#include "sandec.h"

#ifndef _max
#define _max(a,b) ((a) > (b) ? (a) : (b))
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)
#define be32_to_cpu(x) bswap_32(x)
#define be16_to_cpu(x) bswap_16(x)

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

#define be32_to_cpu(x)  (x)
#define be16_to_cpu(x)  (x)
#define le32_to_cpu(x)  bswap_32(x)
#define le16_to_cpu(x)  bswap_16(x)

#else

#error "unknown endianness"

#endif

// chunk ids
#define ANIM	0x414e494d
#define AHDR	0x41484452
#define FRME	0x46524d45
#define NPAL	0x4e50414c
#define FOBJ	0x464f424a
#define IACT	0x49414354
#define TRES	0x54524553
#define STOR	0x53544f52
#define FTCH	0x46544348
#define XPAL	0x5850414c

// codec47 glyhps
#define GLYPH_COORD_VECT_SIZE 16
#define NGLYPHS 256

// internal context: per-file
struct sanrt {
	uint32_t totalsize;
	uint32_t currframe;
	uint32_t framerate;
	uint32_t maxframe;
	uint16_t FRMEcnt;
	uint16_t w;  // frame width/pitch/stride
	uint16_t h;  // frame height
	uint16_t version;
	uint16_t subid;

	unsigned long fbsize;	// size of the buffers below
	unsigned char *buf0;
	unsigned char *buf1;
	unsigned char *buf2;
	unsigned char *buf3;	// aux buffer for "STOR" and "FTCH"
	unsigned char *buf;	// baseptr
	int32_t lastseq;
	uint32_t rotate;
	int to_store;

	uint32_t samplerate;
	uint32_t iactpos;
	uint8_t iactbuf[4096];	// for IACT chunks
	uint32_t palette[256];	// ABGR
	int16_t deltapal[768];	// for XPAL chunks
};

// internal context: static stuff.
struct sanctx {
	struct sanio *io;
	int32_t _bsz;		// current block size
	int errdone;		// error or done.

	// codec47 stuff
	int8_t glyph4x4[NGLYPHS][16];
	int8_t glyph8x8[NGLYPHS][64];

	struct sanrt rt;	// dynamic context, reset on san_open()
};


/******************************************************************************
 * SAN Codec47 Glyph setup, taken from ffmpeg
 * https://git.ffmpeg.org/gitweb/ffmpeg.git/blob_plain/HEAD:/libavcodec/sanm.c
 */

static const int8_t glyph4_x[GLYPH_COORD_VECT_SIZE] = {
	0, 1, 2, 3, 3, 3, 3, 2, 1, 0, 0, 0, 1, 2, 2, 1
};

static const int8_t glyph4_y[GLYPH_COORD_VECT_SIZE] = {
	0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 2, 1, 1, 1, 2, 2
};

static const int8_t glyph8_x[GLYPH_COORD_VECT_SIZE] = {
	0, 2, 5, 7, 7, 7, 7, 7, 7, 5, 2, 0, 0, 0, 0, 0
};

static const int8_t glyph8_y[GLYPH_COORD_VECT_SIZE] = {
	0, 0, 0, 0, 1, 3, 4, 6, 7, 7, 7, 7, 6, 4, 3, 1
};

static const int8_t motion_vectors[256][2] = {
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

static enum GlyphEdge which_edge(int x, int y, int edge_size)
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

static enum GlyphDir which_direction(enum GlyphEdge edge0, enum GlyphEdge edge1)
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
		enum GlyphEdge edge0 = which_edge(x0, y0, side_length);

		for (j = 0; j < GLYPH_COORD_VECT_SIZE; j++, pglyph += glyph_size) {
			int x1 = xvec[j];
			int y1 = yvec[j];
			enum GlyphEdge edge1 = which_edge(x1, y1, side_length);
			enum GlyphDir dir = which_direction(edge0, edge1);
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


static inline int readX(struct sanctx *ctx, void *dst, uint32_t sz)
{
	int ret = ctx->io->ioread(ctx->io->ioctx, dst, sz);
	if (ret == sz) {
		ctx->_bsz -= ret;
		return 0;
	}
	return 1;
}

static inline int read8(struct sanctx *ctx, uint8_t *out)
{
	int ret = ctx->io->ioread(ctx->io->ioctx, out, 1);
	if (ret == 1) {
		ctx->_bsz -= 1;
		return 0;
	}
	return 1;
}

static inline int readLE16(struct sanctx *ctx, uint16_t *out)
{
	int ret = ctx->io->ioread(ctx->io->ioctx, out, 2);
	if (ret == 2) {
		*out = le16_to_cpu(*out);
		ctx->_bsz -= 2;
		return 0;
	}
	return 1;
}

static inline int readLE32(struct sanctx *ctx, uint32_t *out)
{
	int ret = ctx->io->ioread(ctx->io->ioctx, out, 4);
	if (ret == 4) {
		*out = le32_to_cpu(*out);
		ctx->_bsz -= 4;
		return 0;
	}
	return 1;
}

// consume unused bytes in the stream (wrt. chunk size)
static inline void san_read_unused(struct sanctx *ctx)
{
	int32_t t;
	uint8_t v;

	t = ctx->_bsz;
	while (t-- > 0) {
		if (read8(ctx, &v))
			return;
	}
}

#define _READ(x, y, z, ctx)			\
	do {					\
		if (read##x(ctx, y))		\
			return z;		\
	} while (0)

static int readtag(struct sanctx *ctx, uint32_t *outtag, uint32_t *outsize)
{
	uint32_t v[2];
	int ret;

	ret = readX(ctx, v, 8);
	if (ret)
		return 43;

	*outtag = be32_to_cpu(v[0]);
	*outsize = be32_to_cpu(v[1]);

	return 0;
}

static int read_palette(struct sanctx *ctx)
{
	struct sanrt *rt = &ctx->rt;
	uint32_t *pal = rt->palette;
	uint8_t t[12];
	int i = 0;

	while (i < 256) {
		if(readX(ctx, t, 12))
			return 42;
		*pal++ = 0xff << 24 | t [2] << 16 | t[ 1] << 8 | t[0];
		*pal++ = 0xff << 24 | t [5] << 16 | t[ 4] << 8 | t[3];
		*pal++ = 0xff << 24 | t [8] << 16 | t[ 7] << 8 | t[6];
		*pal++ = 0xff << 24 | t[11] << 16 | t[10] << 8 | t[9];
		i += 4;
	}
	return 0;
}

/* keyframe with halved horizontal and vertical resolution */
static int codec47_comp1(struct sanctx *ctx, uint8_t *dst, uint16_t w, uint16_t h)
{
	uint8_t val;

	for (unsigned int j = 0; j < h; j += 2) {
		for (unsigned int i = 0; i < w; i += 2) {
			_READ(8, &val, 41, ctx);
			dst[i] = dst[i + 1] = dst[w + i] = dst[w + i + 1] = val;
		}
		dst += w * 2;
	}
	return 0;
}

static int codec47_block(struct sanctx *ctx, uint8_t *dst, uint8_t *p1, uint8_t *p2, uint16_t w, uint8_t *coltbl, uint16_t size)
{
	uint8_t code, col[2], c;
	uint16_t i, j;
	int8_t *pglyph;

	_READ(8, &code, 35, ctx);
	if (code >= 0xF8) {
		switch (code) {
		case 0xff:
			if (size == 2) {
				uint32_t v32;
				_READ(LE32, &v32, 36, ctx);
				dst[w+1] = v32 >> 24;
				dst[w] = v32 >> 16;
				dst[1] = v32 >>  8;
				dst[0] = v32;
			} else {
				size >>= 1;
				codec47_block(ctx, dst, p1, p2, w, coltbl, size);
				codec47_block(ctx, dst + size, p1 + size, p2 + size, w, coltbl, size);
				dst += (size * w);
				p1 += (size * w);
				p2 += (size * w);
				codec47_block(ctx, dst, p1, p2, w, coltbl, size);
				codec47_block(ctx, dst + size, p1 + size, p2 + size, w, coltbl, size);
			}
			break;
		case 0xfe:
			_READ(8, &c, 37, ctx);
			for (i = 0; i < size; i++)
				memset(dst + (i * w), c, size);
			break;
		case 0xfd:
			_READ(8, &code, 38, ctx);
			_READ(8, &col[0], 39, ctx);
			_READ(8, &col[1], 40, ctx);
			pglyph = (size == 8) ? ctx->glyph8x8[code] : ctx->glyph4x4[code];
			for (i = 0; i < size; i++) {
				for (j = 0; j < size; j++) {
					dst[j + (i * w)] = col[!*pglyph++];
				}
			}
			break;
		case 0xfc:
			for (i = 0; i < size; i++) {
				memcpy(dst + (i * w), p1 + (i * w), size);
			}
			break;
		default:
			c = coltbl[code & 7];
			for (i = 0; i < size; i++) {
				memset(dst + (i * w), c, size);
			}
		}
	} else {
		const int8_t mvx = motion_vectors[code][0];
		const int8_t mvy = motion_vectors[code][1];
		for (i = 0; i < size; i++) {
			memcpy(dst + (i * w), p2 + mvx + ((mvy + i) * w), size);
		}
	}
	return 0;
}

static int codec47_comp2(struct sanctx *ctx, uint8_t *dst, uint16_t w, uint16_t h, uint8_t *coltbl)
{
	uint8_t *b1 = ctx->rt.buf1, *b2 = ctx->rt.buf2;
	unsigned int i, j;
	int ret;

	for (j = 0; j < h; j += 8) {
		for (i = 0; i < w; i += 8) {
			ret = codec47_block(ctx, dst + i, b1 + i, b2 + i, w, coltbl, 8);
			if (ret)
				return ret;
		}
		dst += (w * 8);
		b1 += (w * 8);
		b2 += (w * 8);
	}
	return 0;
}

static int codec47_comp5(struct sanctx *ctx, uint8_t *dst, uint32_t left)
{
	uint8_t opc, rlen, col;

	while (left) {
		_READ(8, &opc, 32, ctx);
		rlen = (opc >> 1) + 1;
		if (rlen > left)
			rlen = left;
		if (opc & 1) {
			_READ(8, &col, 33, ctx);
			memset(dst, col, rlen);
		} else {
			if (readX(ctx, dst, rlen))
				return 34;
		}
		dst += rlen;
		left -= rlen;
	}
	return 0;
}

static int codec47(struct sanctx *ctx, uint32_t size, uint16_t w, uint16_t h, uint16_t top, uint16_t left)
{
	/* the codec47_block() compression path accesses 8 bytes between "skip"
	 * and "decsize" fields. Read the whole 26 byte header block, pass it
	 * around and read our data out of it.
	 */
	uint8_t headtable[32], *dst, comp, newrot, skip;
	uint32_t decsize;
	uint16_t seq;
	int ret;

	/* read the whole header; start dest at offset 2 to align the 32bit read
	 * at table offset 14 to a 32bit boundary.  The codec47_block() code
	 * accesses this table with 1-byte reads so we're good.
	 */
	if (readX(ctx, headtable + 2, 26))
		return 29;

	seq =    le16_to_cpu(*(uint16_t *)(headtable + 2 + 0));
	comp =   headtable[2 + 2];
	newrot = headtable[2 + 3];
	skip =   headtable[2 + 4];
	decsize = le32_to_cpu(*(uint32_t *)(headtable + 2 + 14));

	if (seq == 0) {
		ctx->rt.lastseq = -1;
		memset(ctx->rt.buf1, headtable[2 + 12], ctx->rt.fbsize);
		memset(ctx->rt.buf2, headtable[2 + 13], ctx->rt.fbsize);
	}
	if (skip & 1) {
		if (readX(ctx, NULL, 0x8080))
			return 30;
	}

	ret = 0;
	dst = ctx->rt.buf0 + left + (top * w);
	switch (comp) {
	case 0:	ret = readX(ctx, dst, w * h); break;
	case 1:	ret = codec47_comp1(ctx, dst, w, h); break;
	case 2:	if (seq == (ctx->rt.lastseq + 1)) {
			ret = codec47_comp2(ctx, dst, w, h, headtable + 10);
		}
		break;
	case 3:	memcpy(ctx->rt.buf0, ctx->rt.buf2, ctx->rt.fbsize); break;
	case 4:	memcpy(ctx->rt.buf0, ctx->rt.buf1, ctx->rt.fbsize); break;
	case 5:	ret = codec47_comp5(ctx, dst, decsize); break;
	default: ret = 31;
	}

	ctx->rt.rotate = (seq == ctx->rt.lastseq + 1) ? newrot : 0;
	ctx->rt.lastseq = seq;

	return ret;
}

static int codec1(struct sanctx *ctx, uint32_t size, uint16_t w, uint16_t h, uint16_t top, uint16_t left)
{
	uint8_t *dst, code, col;
	uint16_t rlen, dlen;
	int pos, i, j;

	dst = ctx->rt.buf0 + (top * w);

	for (i = 0; i < h; i++) {
		pos = 0;
		_READ(LE16, &dlen, 25, ctx);
		while (dlen) {
			_READ(8, &code, 26, ctx); dlen--;
			rlen = (code >> 1) + 1;
			if (code & 1) {
				_READ(8, &col, 27, ctx); dlen--;
				if (col)
					memset(dst, col, rlen);
				pos += rlen;
			} else {
				for (j = 0; j < rlen; j++) {
					_READ(8, &col, 28, ctx); dlen--;
					if (col)
						dst[pos] = col;
					pos++;
				}
			}
		}
		dst += w;
	}

	ctx->rt.rotate = 0;
	return 0;
}

static int fobj_alloc_buffers(struct sanrt *rt, uint16_t w, uint16_t h, uint8_t bpp)
{
	const uint32_t bs = w * h * bpp;
	unsigned char *b;

	/* codec47 requires up to 4 buffers the size of the image.
	 * a front buffer, 2 work buffers and an aux buffer to occasionally
	 * save the frontbuffer to/from.
	 */
	b = (unsigned char *)malloc(bs * 4);
	if (!b)
		return 1;

	if (rt->buf)
		free(rt->buf);

	rt->buf = b;
	rt->buf0 = b;
	rt->buf1 = rt->buf0 + bs;
	rt->buf2 = rt->buf1 + bs;
	rt->buf3 = rt->buf2 + bs;
	rt->fbsize = bs;
	rt->w = w;
	rt->h = h;

	return 0;
}

static int handle_FOBJ(struct sanctx *ctx, uint32_t size)
{
	uint16_t codec, left, top, w, h, buf16[16];
	struct sanrt *rt = &ctx->rt;
	int ret;

	if (size < 16)
		return 21;

	// need to track read size since not all bytes of "size" are always consumed
	ctx->_bsz = size;		// init byte tracking
	if (readX(ctx, buf16, 14))	/* 5*2 + 4 */
		return 22;
	codec = le16_to_cpu(buf16[0]);
	left  = le16_to_cpu(buf16[1]);
	top   = le16_to_cpu(buf16[2]);
	w     = le16_to_cpu(buf16[3]);
	h     = le16_to_cpu(buf16[4]);
	/* 32bit unknown value */

	ret = 0;
	if ((rt->w < (left + w)) || (rt->h < (top + h))) {
		ret = fobj_alloc_buffers(rt, _max(rt->w, left + w), _max(rt->h, top + h), 1);
	}
	if (ret != 0)
		return 23;

	switch (codec) {
	case 1:
	case 3: ret = codec1(ctx, size - 14, w, h, top, left); break;
	case 47:ret = codec47(ctx, size - 14, w, h, top, left); break;
	default: ret = 24;
	}

	san_read_unused(ctx);

	return ret;
}

static int handle_NPAL(struct sanctx *ctx, uint32_t size)
{
	int ret;

	ctx->_bsz = size;		// init byte tracking
	ret = read_palette(ctx);
	san_read_unused(ctx);

	return ret;
}

static inline uint8_t _u8clip(int a)
{
	if (a > 255) return 255;
	else if (a < 0) return 0;
	else return a;
}

static int handle_XPAL(struct sanctx *ctx, uint32_t size)
{
	uint32_t t32, *pal = ctx->rt.palette;
	uint16_t dp;
	int i, j, ret, t2[3];

	ret = 0;
	ctx->_bsz = size;		// init byte tracking

	_READ(LE16, &dp, 17, ctx);	// dummy
	_READ(LE16, &dp, 18, ctx);	// command

	if (dp == 256) {
		if (readX(ctx, &dp, 2))	// dummy
			return 19;
		i = 0;
		while (i < 768) {
			t32 = *pal;
			t2[0] = (t32 >>  0) & 0xff;
			t2[1] = (t32 >>  8) & 0xff;
			t2[2] = (t32 >> 16) & 0xff;
			for (j = 0; j < 3; j++) {
				int cl = (t2[j] << 7) + le16_to_cpu(ctx->rt.deltapal[i++]);
				t2[j] = _u8clip(cl >> 7);
			}
			*pal++ = 0xff << 24 | (t2[2] & 0xff) << 16 | (t2[1] & 0xff) << 8 | (t2[0]  & 0xff);
		}

	} else {
		if (readX(ctx, ctx->rt.deltapal, 768 * 2))
			return 20;
		if (dp == 512) {
			ret = read_palette(ctx);
		}
	}

	san_read_unused(ctx);

	return ret;
}

static int handle_IACT(struct sanctx *ctx, uint32_t size)
{
	uint8_t v1, v2, v3, v4, *dst, *src, *src2, *inbuf, outbuf[4096];
	int16_t len, v16;
	uint16_t p[10];
	uint32_t datasz;
	int count, ret;

	datasz = size - 18;

	// size is always a multiple of 2 in the stream, even if the tag reports
	// otherwise.
	if (size & 1)
		size += 1;

	ctx->_bsz = size;		// init byte tracking
	if (readX(ctx, p + 1, 18))	/* +2bytes to align the 32bit read for vv */
		return 13;

#if 0
	uint32_t vv = *(uint32_t *)(&p[8]);
	printf("IACT sz %u code %u flags %u unknown %u uid %u trkid %u frame %u"
	       " maxframes %u data_left_in_track %u iactpos %d\n", size,
	       le16_tp_cpu(p[1]), le16_tp_cpu(p[2]), le16_tp_cpu(p[3]),
	       le16_tp_cpu(p[4]), le16_tp_cpu(p[5]), le16_tp_cpu(p[6]),
	       le16_tp_cpu(p[7]), le32_tp_cpu(vv), ctx->rt.iactpos);
#endif

	if (le16_to_cpu(p[1]) != 8  ||
	    le16_to_cpu(p[2]) != 46 ||
	    le16_to_cpu(p[3]) != 0  ||
	    le16_to_cpu(p[4]) != 0) {
		ret = 14;
		goto out;
	}

	inbuf = malloc(datasz);
	if (!inbuf) {
		ret = 15;
		goto out;
	}
	if (readX(ctx, inbuf, datasz)) {
		ret = 16;
		goto out1;
	}

	ret = 0;
	src = inbuf;

	/* algorithm taken from ScummVM/engines/scumm/smush/smush_player.cpp.
	 * I only changed the output generator for LSB samples (while the source
	 * and ScummVM output MSB samples).
	 */
	while (datasz > 0) {
		if (ctx->rt.iactpos >= 2) {
			len = be16_to_cpu(*(uint16_t *)ctx->rt.iactbuf) + 2 - ctx->rt.iactpos;
			if (len > datasz) {  // continued in next IACT chunk.
				memcpy(ctx->rt.iactbuf + ctx->rt.iactpos, src, datasz);
				ctx->rt.iactpos += datasz;
				datasz = 0;
			} else {
				memcpy(ctx->rt.iactbuf + ctx->rt.iactpos, src, len);
				dst = outbuf;
				src2 = ctx->rt.iactbuf + 2;
				v1 = *src2++;
				v2 = v1 >> 4;
				v1 &= 0x0f;
				count = 1024;
				do {
					v3 = *src2++;
					if (v3 == 0x80) {
						// endian-swap BE16 samples
						v4 = *src2++;
						*dst++ = *src2++;
						*dst++ = v4;
					} else {
						v16 = (int8_t)v3 << v2;
						*dst++ = (int8_t)(v16) & 0xff;
						*dst++ = (int8_t)(v16 >> 8) & 0xff;
					}
					v3 = *src2++;
					if (v3 == 0x80) {
						// endian-swap BE16 samples
						v4 = *src2++;
						*dst++ = *src2++;
						*dst++ = v4;
					} else {
						v16 = (int8_t)v3 << v1;
						*dst++ = (int8_t)(v16) & 0xff;
						*dst++ = (int8_t)(v16 >> 8) & 0xff;

					}
				} while (--count);
				ret = ctx->io->queue_audio(ctx->io->avctx, outbuf, 4096);
				if (ret)
					goto out1;
				datasz -= len;
				src += len;
				ctx->rt.iactpos = 0;
			}
		} else {
			if (datasz > 1 && ctx->rt.iactpos == 0) {
				*ctx->rt.iactbuf = *src++;
				ctx->rt.iactpos = 1;
				datasz--;
			}
			*(ctx->rt.iactbuf + ctx->rt.iactpos) = *src++;
			ctx->rt.iactpos++;
			datasz--;
		}
	}
out1:
	free(inbuf);
out:
	san_read_unused(ctx);
	return ret;
}

// subtitles
static int handle_TRES(struct sanctx *ctx, uint32_t size)
{
	uint16_t tres[9];
	int ret;

	ctx->_bsz = size;		// init byte tracking
	ret = readX(ctx, tres, 18);

#if 0
	_READ(LE16, &px, 1, ctx);
	_READ(LE16, &py, 1, ctx);
	_READ(LE16, &f, 1, ctx);
	_READ(LE16, &l, 1, ctx);
	_READ(LE16, &t, 1, ctx);
	_READ(LE16, &w, 1, ctx);
	_READ(LE16, &h, 1, ctx);
	_READ(LE16, &strid, 1, ctx); // dummy
	_READ(LE16, &strid, 1, ctx); // real strid
#endif
	ctx->rt.subid = tres[8];
	san_read_unused(ctx);

	return ret;
}

static int handle_STOR(struct sanctx *ctx, uint32_t size)
{
	ctx->_bsz = size;		// init byte tracking
	ctx->rt.to_store = 1;
	san_read_unused(ctx);
	return 0;
}

static int handle_FTCH(struct sanctx *ctx, uint32_t size)
{
	ctx->_bsz = size;		// init byte tracking
	memcpy(ctx->rt.buf0, ctx->rt.buf3, ctx->rt.fbsize);
	san_read_unused(ctx);
	return 0;
}

static int handle_FRME(struct sanctx *ctx, uint32_t size)
{
	struct sanrt *rt = &ctx->rt;
	uint32_t cid, csz;
	uint8_t v;
	int ret;

	ret = 0;
	while ((size > 3) && (ret == 0)) {
		ret = readtag(ctx, &cid, &csz);
		if (ret)
			return 9;
		if (csz > size)
			return 10;
		switch (cid)
		{
		case NPAL: ret = handle_NPAL(ctx, csz); break;
		case FOBJ: ret = handle_FOBJ(ctx, csz); break;
		case IACT: ret = handle_IACT(ctx, csz); break;
		case TRES: ret = handle_TRES(ctx, csz); break;
		case STOR: ret = handle_STOR(ctx, csz); break;
		case FTCH: ret = handle_FTCH(ctx, csz); break;
		case XPAL: ret = handle_XPAL(ctx, csz); break;
		default:
			ret = 11;
			break;
		}
		if (csz & 1)
			csz += 1;
		size -= csz + 8;
	}

	if (size < 4 && ret == 0) {
		// OK Case: most bytes consumed, no errors.
		if (rt->to_store) {
			memcpy(rt->buf3, rt->buf0, rt->fbsize);
		}

		// copy rt->buf0 to output
		ret = ctx->io->queue_video(ctx->io->avctx, rt->buf0, rt->fbsize,
					   rt->w, rt->h, rt->palette, rt->subid);

		if (rt->rotate) {
			unsigned char *tmp;
			if (rt->rotate == 2) {
				tmp = rt->buf1;
				rt->buf1 = rt->buf2;
				rt->buf2 = tmp;
			}
			tmp = rt->buf2;
			rt->buf2 = rt->buf0;
			rt->buf0 = tmp;
		}

		// consume any unread bytes
		while (size--)
			if (read8(ctx, &v))
				return 12;

		rt->to_store = 0;
		rt->currframe++;
		rt->rotate = 0;
		rt->subid = 0;
	}

	return ret;
}

static int handle_AHDR(struct sanctx *ctx, uint32_t size)
{
	struct sanrt *rt = &ctx->rt;
	uint16_t v16[3];
	uint32_t v32[5];
	int ret;

	if (size < 768 + 6)
		return 6;		// too small

	ctx->_bsz = size;		// init byte tracking
	ret = readX(ctx, v16, 3 * 2);
	if (ret)
		return 7;
	rt->version = le16_to_cpu(v16[0]);
	rt->FRMEcnt = le16_to_cpu(v16[1]);
	/* unk16 */

	ret = read_palette(ctx);

	if (ctx->_bsz < 20) {
		ret = 8;
	} else {
		ret = readX(ctx, v32, 5 * 4);
		if (ret)
			return 2;
		rt->framerate =  le32_to_cpu(v32[0]);
		rt->maxframe =   le32_to_cpu(v32[1]);
		rt->samplerate = le32_to_cpu(v32[2]);
		/* unk32_1 */
		/* unk32_2 */
		ret = 0;
	}

	san_read_unused(ctx);

	return 0;
}

/******************************************************************************/
/* public interface */

int sandec_decode_next_frame(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	uint32_t cid, csz;
	int ret;

	/* in case of previous error, don't continue, just return it again */
	if (ctx->errdone)
		return ctx->errdone;

	ret = readtag(ctx, &cid, &csz);
	if (ret) {
		if (ctx->rt.currframe == ctx->rt.FRMEcnt)
			ret = -1;
		goto out;
	}

	switch (cid) {
	case FRME: 	ret = handle_FRME(ctx, csz); break;
	default:	ret = 5;
	}

out:
	ctx->errdone = ret;
	return ret;
}

int sandec_init(void **ctxout)
{
	struct sanctx *ctx;

	ctx = malloc(sizeof(struct sanctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(struct sanctx));
	/* set to error state initially until a valid file has been opened */
	ctx->errdone = 44;

	c47_make_glyphs(ctx->glyph4x4[0], glyph4_x, glyph4_y, 4);
	c47_make_glyphs(ctx->glyph8x8[0], glyph8_x, glyph8_y, 8);
	*ctxout = ctx;

	return 0;
}

int sandec_open(void *sanctx, struct sanio *io)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	int ret, ok;
	uint32_t cid;
	uint32_t csz;
	int have_anim = 0;
	int have_ahdr = 0;

	if (!io) {
		ret = 2;
		goto out;
	}
	ctx->io = io;

	/* delete an existing framebuffer */
	if (ctx->rt.buf && ctx->rt.fbsize)
		free(ctx->rt.buf);
	/* force-initialize the dynamic context */
	memset(&ctx->rt, 0, sizeof(struct sanrt));

	while (1) {
		ok = readtag(ctx, &cid, &csz);
		if (ok) {
			ret = 3;
			goto out;
		}

		if (!have_anim) {
			if (cid == ANIM) {
				have_anim = 1;
			}
			continue;
		}
		if (cid == AHDR) {
			have_ahdr = 1;
			break;
		}
	}

	if (have_ahdr)
		ret = handle_AHDR(ctx, csz);
out:
	ctx->errdone = ret;
	return ret;
}

void sandec_exit(void **sanctx)
{
	struct sanctx *ctx = *(struct sanctx **)sanctx;
	if (!ctx)
		return;

	/* delete the framebuffer */
	if (ctx->rt.buf)
		free(ctx->rt.buf);
	memset(&ctx->rt, 0, sizeof(struct sanrt));
	free(ctx);
	*sanctx = NULL;
}

int sandec_get_framerate(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.framerate : -1;
}

int sandec_get_samplerate(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.samplerate : -1;
}

int sandec_get_framecount(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.FRMEcnt : -1;
}

int sandec_get_version(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.version : -1;
}

int sandec_get_currframe(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.currframe : -1;
}
