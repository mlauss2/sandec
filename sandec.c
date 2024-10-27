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
 * https://github.com/clone2727/smushplay/blob/master/codec47.cpp
 */

#include <memory.h>
#include <stdlib.h>
#include "sandec.h"

#ifndef _max
#define _max(a,b) ((a) > (b) ? (a) : (b))
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


#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__


#define be32_to_cpu(x)  (x)
#define be16_to_cpu(x)  (x)
#define le32_to_cpu(x)  bswap_32(x)
#define le16_to_cpu(x)  bswap_16(x)

/* chunk identifiers BE */
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


#else

#error "unknown endianness"

#endif

/* codec47 glyhps */
#define GLYPH_COORD_VECT_SIZE 16
#define NGLYPHS 256

/* internal context: per-file */
struct sanrt {
	uint8_t *fcache;	/* 8 one cached FRME object		*/
	uint8_t *buf0;		/* 8 current front buffer		*/
	uint8_t *buf1;		/* 8 c47 delta buffer 1			*/
	uint8_t *buf2;		/* 8 c47 delta buffer 2			*/
	uint16_t w;		/* 2 image width/pitch			*/
	uint16_t h;		/* 2 image height			*/
	int16_t  lastseq;	/* 2 c47 last sequence id		*/
	uint16_t rotate;	/* 2 c47 buffer rotation code		*/
	uint16_t subid;		/* 2 subtitle message number		*/
	uint16_t to_store;	/* 2 STOR encountered			*/
	uint16_t currframe;	/* 2 current frame index		*/
	uint16_t iactpos;	/* 2 IACT buffer write pointer		*/
	uint8_t *iactbuf;	/* 8 4kB for IACT chunks 		*/
	uint8_t *c47ipoltbl;	/* 8 c47 interpolation table Compression 1 */
	uint8_t *buf3;		/* 8 aux buffer for "STOR" and "FTCH"	*/
	int16_t  *deltapal;	/* 8 768x 16bit for XPAL chunks		*/
	uint32_t *palette;	/* 8 256x ABGR				*/
	uint8_t  *buf;		/* 8 fb baseptr				*/
	uint32_t fbsize;	/* 4 size of the framebuffers		*/
	uint32_t framerate;	/* 4 fps				*/
	uint32_t samplerate;	/* 4 audio samplerate in Hz		*/
	uint16_t FRMEcnt;	/* 2 number of FRMEs in SAN		*/
	uint16_t version;	/* 2 SAN version number			*/
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

static const int8_t c47_motion_vectors[256][2] = {
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

static inline int read_source(struct sanctx *ctx, void *dst, uint32_t sz)
{
	return !(ctx->io->ioread(ctx->io->ioctx, dst, sz));
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

static void codec47_comp1(struct sanctx *ctx, uint8_t *src, uint8_t *dst_in, uint16_t w, uint16_t h)
{
	/* input data is i-frame with half width and height. combining 2 pixels
	 * into a 16bit value, one can then use this value as an index into
	 * the interpolation table to get the missing color between 2 pixels.
	 */
	uint8_t *itbl = ctx->rt.c47ipoltbl, *dst, p8, p82;
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

static uint8_t* codec47_block(struct sanctx *ctx, uint8_t *src, uint8_t *dst, uint8_t *p1, uint8_t *p2, uint16_t w, uint8_t *coltbl, uint16_t size)
{
	uint8_t code, col[2], c;
	uint16_t i, j;
	int8_t *pglyph;

	code = *src++;
	if (code >= 0xF8) {
		switch (code) {
		case 0xff:
			if (size == 2) {
				dst[0] = *src++; dst[1] = *src++;
				dst[w] = *src++; dst[w + 1] = *src++;
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
				memset(dst + (i * w), c, size);
			break;
		case 0xfd:
			code = *src++;
			col[0] = *src++;
			col[1] = *src++;
			pglyph = (size == 8) ? ctx->c47_glyph8x8[code] : ctx->c47_glyph4x4[code];
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
		const int8_t mvx = c47_motion_vectors[code][0];
		const int8_t mvy = c47_motion_vectors[code][1];
		for (i = 0; i < size; i++) {
			memcpy(dst + (i * w), p2 + mvx + ((mvy + i) * w), size);
		}
	}
	return src;
}

static void codec47_comp2(struct sanctx *ctx, uint8_t *src, uint8_t *dst, uint16_t w, uint16_t h, uint8_t *coltbl)
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

static void codec47_comp5(struct sanctx *ctx, uint8_t *src, uint8_t *dst, uint32_t left)
{
	uint8_t opc, rlen, col;

	while (left) {
		opc = *src++;
		rlen = (opc >> 1) + 1;
		if (rlen > left)
			rlen = left;
		if (opc & 1) {
			col = *src++;
			memset(dst, col, rlen);
		} else {
			memcpy(dst, src, rlen);
			src += rlen;
		}
		dst += rlen;
		left -= rlen;
	}
}

static void codec47_itable(struct sanctx *ctx, uint8_t **src2)
{
	uint8_t *itbl, *p1, *p2, *src = *src2;
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
	*src2 = src;
}

static int codec47(struct sanctx *ctx, uint8_t *src, uint16_t w, uint16_t h, uint16_t top, uint16_t left)
{
	uint8_t *insrc = src, *dst, comp, newrot, flag;
	uint32_t decsize;
	uint16_t seq;
	int ret;

	seq =    le16_to_cpu(*(uint16_t *)(src + 0));
	comp =   src[2];
	newrot = src[3];
	flag =   src[4];
	/* this 32bit value is not always aligned at a 4-byte boundary! */
	decsize  = le16_to_cpu(*(uint16_t *)(src + 14));
	decsize |= le16_to_cpu(*(uint16_t *)(src + 16)) << 16;

	if (seq == 0) {
		ctx->rt.lastseq = -1;
		memset(ctx->rt.buf1, src[12], ctx->rt.fbsize);
		memset(ctx->rt.buf2, src[13], ctx->rt.fbsize);
	}
	src += 26;
	if (flag & 1) {
		codec47_itable(ctx, &src);
	}

	ret = 0;
	dst = ctx->rt.buf0 + left + (top * w);
	switch (comp) {
	case 0:	memcpy(dst, src, w * h); break;
	case 1:	codec47_comp1(ctx, src, dst, w, h); break;
	case 2:	if (seq == (ctx->rt.lastseq + 1)) {
			codec47_comp2(ctx, src, dst, w, h, insrc + 8);
		}
		break;
	case 3:	memcpy(ctx->rt.buf0, ctx->rt.buf2, ctx->rt.fbsize); break;
	case 4:	memcpy(ctx->rt.buf0, ctx->rt.buf1, ctx->rt.fbsize); break;
	case 5:	codec47_comp5(ctx, src, dst, decsize); break;
	default: ret = 16;
	}

	ctx->rt.rotate = (seq == ctx->rt.lastseq + 1) ? newrot : 0;
	ctx->rt.lastseq = seq;

	return ret;
}

static void codec1(struct sanctx *ctx, uint8_t *src, uint16_t w, uint16_t h, uint16_t top, uint16_t left)
{
	uint8_t *dst, code, col;
	uint16_t rlen, dlen;
	int i, j;

	dst = ctx->rt.buf0 + (top * w) + left;
	for (i = 0; i < h; i++) {
		dlen = le16_to_cpu(*(uint16_t *)src); src += 2;
		while (dlen > 0) {
			code = *src++; dlen--;
			rlen = (code >> 1) + 1;
			if (code & 1) {
				col = *src++; dlen--;
				if (col)
					memset(dst, col, rlen);
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

static int handle_FOBJ(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint16_t codec, left, top, w, h;
	struct sanrt *rt = &ctx->rt;
	int ret;

	codec = le16_to_cpu(*(uint16_t *)(src + 0));
	left  = le16_to_cpu(*(uint16_t *)(src + 2));
	top   = le16_to_cpu(*(uint16_t *)(src + 4));
	w     = le16_to_cpu(*(uint16_t *)(src + 6));
	h     = le16_to_cpu(*(uint16_t *)(src + 8));
	/* 32bit unknown value */

	ret = 0;
	if ((rt->w < (left + w)) || (rt->h < (top + h))) {
		ret = fobj_alloc_buffers(rt, _max(rt->w, left + w), _max(rt->h, top + h), 1);
	}
	if (ret != 0)
		return 14;

	switch (codec) {
	case 1:
	case 3: codec1(ctx, src + 14, w, h, top, left); break;
	case 47:ret = codec47(ctx, src + 14, w, h, top, left); break;
	default: ret = 15;
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

static int handle_XPAL(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	const uint16_t cmd = be16_to_cpu(*(uint16_t *)(src + 2));
	uint32_t t32, *pal = ctx->rt.palette;
	int i, j, t2[3];


	src += 4;

	/* cmd1: apply delta */
	if (cmd == 1) {
		i = 0;
		while (i < 768) {
			t32 = *pal;
			t2[0] = (t32 >>  0) & 0xff;
			t2[1] = (t32 >>  8) & 0xff;
			t2[2] = (t32 >> 16) & 0xff;
			for (j = 0; j < 3; j++) {
				int cl = (t2[j] * 129) + le16_to_cpu(ctx->rt.deltapal[i++]);
				t2[j] = _u8clip(cl / 128);
			}
			*pal++ = 0xff << 24 | (t2[2] & 0xff) << 16 | (t2[1] & 0xff) << 8 | (t2[0]  & 0xff);
		}
	/* cmd2: read deltapal values */
	} else if (cmd == 2) {
		memcpy(ctx->rt.deltapal, src, 768 * 2);
		if (size > (768 * 2 + 4))
			read_palette(ctx, src + (768 * 2));
	} else {
		return 13;		/*  unknown XPAL cmd */
	}
	return 0;
}

static int handle_IACT(struct sanctx *ctx, uint32_t size, uint8_t *isrc)
{
	uint8_t v1, v2, v3, v4, *dst, *src, *src2, outbuf[4096];
	uint16_t count, len, *p = (uint16_t *)isrc;
	uint32_t datasz = size - 18;
	int16_t v16;

	/* this code only works when these parameters are met: */
	if (le16_to_cpu(p[0]) != 8  ||
	    le16_to_cpu(p[1]) != 46 ||
	    le16_to_cpu(p[2]) != 0  ||
	    le16_to_cpu(p[3]) != 0) {
		return 12;
	}

	src = isrc + 18;

	/* algorithm taken from ScummVM/engines/scumm/smush/smush_player.cpp.
	 * I only changed the output generator for LSB samples (while the source
	 * and ScummVM output MSB samples).
	 */
	while (datasz > 0) {
		if (ctx->rt.iactpos >= 2) {
			len = be16_to_cpu(*(uint16_t *)ctx->rt.iactbuf) + 2 - ctx->rt.iactpos;
			if (len > datasz) {  /* continued in next IACT chunk. */
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
						/* endian-swap BE16 samples */
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
						/* endian-swap BE16 samples */
						v4 = *src2++;
						*dst++ = *src2++;
						*dst++ = v4;
					} else {
						v16 = (int8_t)v3 << v1;
						*dst++ = (int8_t)(v16) & 0xff;
						*dst++ = (int8_t)(v16 >> 8) & 0xff;

					}
				} while (--count);
				int ret = ctx->io->queue_audio(ctx->io->avctx, outbuf, 4096);
				if (ret)
					return ret;
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

	return 0;
}

/* subtitles */
static void handle_TRES(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	uint16_t *tres = (uint16_t *)src;

#if 0
	px = tres[0];
	py = tres[1];
	f = tres[2];
	l = tres[3];
	t = tres[4];
	w = tres[5];
	h = tres[6];
	strid1 = tres[7];
#endif
	ctx->rt.subid = le16_to_cpu(tres[8]);
}

static void handle_STOR(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	ctx->rt.to_store = 1;
}

static void handle_FTCH(struct sanctx *ctx, uint32_t size, uint8_t *src)
{
	memcpy(ctx->rt.buf0, ctx->rt.buf3, ctx->rt.fbsize);
}

static int handle_FRME(struct sanctx *ctx, uint32_t size)
{
	struct sanrt *rt = &ctx->rt;
	uint8_t *src = rt->fcache;
	uint32_t cid, csz;
	int ret;

	if (read_source(ctx, src, size))
		return 10;

	ret = 0;
	while ((size > 7) && (ret == 0)) {
		cid = *(uint16_t *)(src + 0) | (*(uint16_t *)(src + 2)) << 16;
		csz = *(uint16_t *)(src + 4) | (*(uint16_t *)(src + 6)) << 16;
		csz = be32_to_cpu(csz);

		src += 8;
		size -= 8;

		if (csz > size)
			return 17;

		switch (cid)
		{
		case NPAL: handle_NPAL(ctx, csz, src); break;
		case FOBJ: ret = handle_FOBJ(ctx, csz, src); break;
		case IACT: ret = handle_IACT(ctx, csz, src); break;
		case TRES: handle_TRES(ctx, csz, src); break;
		case STOR: handle_STOR(ctx, csz, src); break;
		case FTCH: handle_FTCH(ctx, csz, src); break;
		case XPAL: ret = handle_XPAL(ctx, csz, src); break;
		default:
			ret = 11;
			break;
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
		/* STOR */
		if (rt->to_store)
			memcpy(rt->buf3, rt->buf0, rt->fbsize);

		ret = ctx->io->queue_video(ctx->io->avctx, rt->buf0, rt->fbsize,
					   rt->w, rt->h, rt->palette, rt->subid);

		if (rt->rotate) {
			uint8_t *tmp;
			if (rt->rotate == 2) {
				tmp = rt->buf1;
				rt->buf1 = rt->buf2;
				rt->buf2 = tmp;
			}
			tmp = rt->buf2;
			rt->buf2 = rt->buf0;
			rt->buf0 = tmp;
		}

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
	uint8_t *ahbuf, *xbuf;
	uint32_t maxframe;
	int ret;

	if (size < 768 + 26)
		return 5;		/* too small */

	ahbuf = malloc(size);
	if (!ahbuf)
		return 6;

	/* buffer for IACT buffer (4096), Palette (256*4), deltapal (768*2),
	 * and c47 interpolation table (0x10000) */
	xbuf = malloc(4096 + 256 * 4 + 768 * 2 + 0x10000);
	if (!xbuf) {
		ret = 7;
		goto out;
	}
	rt->iactbuf = xbuf;
	rt->palette = (uint32_t *)(xbuf + 4096);
	rt->deltapal = (int16_t *)(xbuf + 4096 + (256 * 4));
	rt->c47ipoltbl = (uint8_t *)(xbuf + 4096 + (256 * 4) + (768 * 2));

	if (read_source(ctx, ahbuf, size))
		return 8;

	rt->version = le16_to_cpu(*(uint16_t *)(ahbuf + 0));
	rt->FRMEcnt = le16_to_cpu(*(uint16_t *)(ahbuf + 2));

	read_palette(ctx, ahbuf + 6);	/* 768 bytes */

	rt->framerate =  le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 0));
	maxframe =       le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 4));
	rt->samplerate = le32_to_cpu(*(uint32_t *)(ahbuf + 6 + 768 + 8));

	/* "maxframe" indicates the maximum size of one FRME object
	 * including chunk ID and chunk size in the stream (usually the first)
	 * plus 1 byte.
	 */
	ret = 0;
	if ((maxframe > 9) && (maxframe < 4 * 1024 * 1024)) {
		maxframe -= 9;
		if (maxframe & 1)
			maxframe += 1;	/* make it even */
		rt->fcache = malloc(maxframe);
		if (!rt->fcache)
			ret = 9;
	}

out:
	if (ret && xbuf)
		free(xbuf);

	free(ahbuf);
	return ret;
}

static void sandec_free_memories(struct sanctx *ctx)
{
	/* delete existing FRME buffer */
	if (ctx->rt.fcache)
		free(ctx->rt.fcache);
	/* delete IACT/palette/deltapal/c47ipoltbl buffer */
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

	/* in case of previous error, don't continue, just return it again */
	if (ctx->errdone)
		return ctx->errdone;

	ret = read_source(ctx, c, 8);
	if (ret) {
		if (ctx->rt.currframe == ctx->rt.FRMEcnt)
			ret = -1;
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

	ctx = malloc(sizeof(struct sanctx));
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

	if (!io) {
		ret = 2;
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
	struct sanctx *ctx = *(struct sanctx **)sanctx;
	if (!ctx)
		return;

	sandec_free_memories(ctx);
	free(ctx);
	*sanctx = NULL;
}

int sandec_get_framerate(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.framerate : 0;
}

int sandec_get_samplerate(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.samplerate : 0;
}

int sandec_get_framecount(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.FRMEcnt : 0;
}

int sandec_get_version(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.version : 0;
}

int sandec_get_currframe(void *sanctx)
{
	struct sanctx *ctx = (struct sanctx *)sanctx;
	return ctx ? ctx->rt.currframe : 0;
}
