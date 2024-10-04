/*
 * SAN ANIM file decoder for Outlaws ".SAN" video files.
 * Outlaws SAN files use SMUSH codec 47 and IACT 22.05kHz 16-bit stereo Audio.
 *
 * Clobbered together from FFmpeg/libavcodec sanm.c decoder and ScummVM
 *  SMUSH player code.
 */

#include <memory.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "sandec.h"
#include "byteswap.h"

#ifndef _max
#define _max(a,b) ((a) > (b) ? (a) : (b))
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



/******************************************************************************
 * SAN Codec47 Glyph setup, taken from ffmpeg
 * https://git.ffmpeg.org/gitweb/ffmpeg.git/blob_plain/HEAD:/libavcodec/sanm.c
 */
#define GLYPH_COORD_VECT_SIZE 16

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
				}
			}
		}
	}
}


/******************************************************************************/


static inline int readX(struct sanrt *rt, void *dst, uint32_t sz)
{
	int ret = rt->io->read(rt->io->ctx, dst, sz);
	if (ret > 0) {
		rt->_bsz -= ret;
	}
	return ret == sz ? 0 : 1;
}

static inline int read8(struct sanrt *rt, uint8_t *out)
{
	int ret = rt->io->read(rt->io->ctx, out, 1);
	if (ret > 0)
		rt->_bsz -= 1;
	return ret > 0 ? 0 : 1;
}

static inline int readLE16(struct sanrt *rt, uint16_t *out)
{
	int ret = rt->io->read(rt->io->ctx, out, 2);
	if (ret > 0) {
		*out = le16_to_cpu(*out);
		rt->_bsz -= 2;
	}
	return ret > 0 ? 0 : 1;
}

static inline int readLE32(struct sanrt *rt, uint32_t *out)
{
	int ret = rt->io->read(rt->io->ctx, out, 4);
	if (ret > 0) {
		*out = le32_to_cpu(*out);
		rt->_bsz -= 4;
	}
	return ret > 0 ? 0 : 1;
}

static inline int readBE16(struct sanrt *rt, uint16_t *out)
{
	int ret = rt->io->read(rt->io->ctx, out, 2);
	if (ret > 0) {
		*out = be16_to_cpu(*out);
		rt->_bsz -= 2;
	}
	return ret > 0 ? 0 : 1;
}

static inline int readBE32(struct sanrt *rt, uint32_t *out)
{
	int ret = rt->io->read(rt->io->ctx, out, 4);
	if (ret > 0) {
		*out = be32_to_cpu(*out);
		rt->_bsz -= 4;
	}
	return ret > 0 ? 0 : 1;
}

// consume unused bytes in the stream (wrt. chunk size)
static inline int san_read_unused(struct sanrt *rt)
{
	uint32_t t;
	uint8_t v;

	t = rt->_bsz;
	while (t--) {
		read8(rt, &v);
	}
}

#define _READ(x, y, z, rt)			\
	do {					\
		if (read##x(rt, y))		\
			return z;		\
	} while (0)

static int readtag(struct sanrt *rt, uint32_t *outtag, uint32_t *outsize)
{
	uint32_t v1, v2;
	int ret;

	ret = readBE32(rt, &v1);
	if (ret)
		return 11;
	ret = readBE32(rt, &v2);
	if (ret)
		return 12;

	*outtag = v1;
	*outsize = v2;

	return 0;
}

// uncompressed full frame
static int codec47_comp0(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h)
{
	for (unsigned int i = 0; i < h; i++) {
		if (readX(rt, dst, w))
			return 1; // error
		dst += w;
	}
	return 0;
}

// full frame in half width and height resolution
static int codec47_comp1(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h)
{
	uint8_t val;

	for (unsigned int j = 0; j < h; j += 2) {
		for (unsigned int i = 0; i < w; i += 2) {
			_READ(8, &val, 1, rt);
			dst[i] = dst[i + 1] = dst[w + i] = dst[w + i + 1] = val;
		}
	}
	return 0;
}

static int codec47_block(struct sanrt *rt, unsigned char *dst, unsigned char *p1, unsigned char *p2, uint16_t w, uint8_t *headtbl, uint16_t size)
{
	uint8_t code, col[2], c, val;
	uint16_t i, j;
	int8_t *pglyph;

	_READ(8, &code, 1, rt);
	if (code >= 0xF8) {
		switch (code) {
		case 0xff:
			if (size == 2) {
				_READ(8, &dst[0], 1, rt);
				_READ(8, &dst[1], 1, rt);
				_READ(8, &dst[w], 1, rt);
				_READ(8, &dst[w+1], 1, rt);
			} else {
				size /= 2;
				codec47_block(rt, dst, p1, p2, w, headtbl, size);
				codec47_block(rt, dst + size, p1 + size, p2 + size, w, headtbl, size);
				dst += (size * w);
				p1 += (size * w);
				p2 += (size * w);
				codec47_block(rt, dst, p1, p2, w, headtbl, size);
				codec47_block(rt, dst + size, p1 + size, p2 + size, w, headtbl, size);
			}
			break;
		case 0xfe:
			_READ(8, &c, 1, rt);
			for (i = 0; i < size; i++)
				memset(dst + (i * w), c, size);
			break;
		case 0xfd:
			_READ(8, &code, 1, rt);
			_READ(8, &col[0], 1, rt);
			_READ(8, &col[1], 1, rt);
			pglyph = (size == 8) ? rt->glyph8x8[code] : rt->glyph4x4[code];
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
			c = headtbl[code & 7];
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

static int codec47_comp2(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h, uint32_t seq, uint8_t *headtbl)
{
	unsigned int i, j;
	unsigned char *b1, *b2;

	if (seq != (rt->lastseq+1))
		return 0;

	b1 = rt->buf1;
	b2 = rt->buf2;

	for (j = 0; j < h; j += 8) {
		for (i = 0; i < w; i += 8) {
			codec47_block(rt, dst + i, b1 + i, b2 + i, w, headtbl + 8, 8);
		}
		dst += (w * 8);
		b1 += (w * 8);
		b2 += (w * 8);
	}
	return 0;
}

// RLE
static int codec47_comp5(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h, uint32_t left)
{
	uint8_t opc, rlen, col;

	while (left) {
		_READ(8, &opc, 1, rt);
		rlen = (opc >> 1) + 1;
		if (opc & 1) {
			_READ(8, &col, 1, rt);
			memset(dst, col, rlen);
		} else {
			if (readX(rt, dst, rlen))
				return 1;
		}
		dst += rlen;
		left -= rlen;
	}
	return 0;
}

static int codec47(struct sanrt *rt, uint32_t size, uint16_t w, uint16_t h, uint16_t top, uint16_t left)
{
	// the codec47_block() compression path accesses parts of this header.
	// instead of the seek/tell file pointer games that the ffmpeg code uses,
	// we just read the whole 26 byte block, pass it around and read our
	// data out of it.
	uint8_t headtable[32];

	// read header
	if (readX(rt, headtable, 26))
		return 1;

	uint16_t seq = *(uint16_t *)(headtable + 0);
	uint8_t comp = headtable[2];
	uint8_t newrot = headtable[3];
	uint8_t skip = headtable[4];
	uint32_t decsize = *(uint32_t *)(headtable + 14);
	unsigned char *dst;
	int ret;

	//printf("C47  seq %4u comp %u newrot %u skip %u decsize %u\n", seq, comp, newrot, skip, decsize);
	if (seq == 0) {
		rt->lastseq = (uint32_t)-1;
		memset(rt->buf1, 0, rt->fbsize);
		memset(rt->buf2, 0, rt->fbsize);
	}
	if (skip & 1) {
		readX(rt, NULL, 0x8080);
	}

	dst = rt->buf0 + left + (top * w);
	switch (comp) {
	case 0:	ret = codec47_comp0(rt, dst, w, h); break;
	case 1: ret = codec47_comp1(rt, dst, w, h); break;
	case 2: ret = codec47_comp2(rt, dst, w, h, seq, headtable); break;
	case 3:	memcpy(rt->buf0, rt->buf2, rt->fbsize); break;
	case 4: memcpy(rt->buf0, rt->buf1, rt->fbsize); break;
	case 5: ret = codec47_comp5(rt, dst, w, h, decsize); break;
	default: ret = 99;
	}

	rt->rotate = (seq == rt->lastseq + 1) ? newrot : 0;
	rt->lastseq = seq;

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
	rt->buf3 = rt->buf1 + bs;
	rt->fbsize = bs;
	rt->w = w;
	rt->h = w;

	return 0;
}

static int handle_FOBJ(struct sanrt *rt, uint32_t size)
{
	uint16_t codec, left, top, w, h;
	uint32_t t1;
	int ret;

	if (size < 12)
		return 71;

	// need to track read size since not all bytes of "size" are always consumed
	rt->_bsz = size;		// init byte tracking
	_READ(LE16, &codec, 72, rt);
	_READ(LE16, &left, 73, rt);
	_READ(LE16, &top, 74, rt);
	_READ(LE16, &w, 75, rt);
	_READ(LE16, &h, 76, rt);
	_READ(LE32, &t1, 77, rt);

//	printf("FOBJ size %d %dx%d at %dx%d, codec %d\n", size, w, h, left, top, codec);

	if (codec =! 47)
		return 78;

	ret = 0;
	if ((rt->w < (left + w)) || (rt->h < (top + h))) {
		ret = fobj_alloc_buffers(rt, _max(rt->w, left + w), _max(rt->h, top + h), 1);
	}
	if (ret != 0)
		return 79;

	ret = codec47(rt, size - 14, w, h, top, left);

	san_read_unused(rt);

	return ret;
}

static int handle_NPAL(struct sanrt *rt, uint32_t size)
{
	uint8_t tmpbuf[4];
	int j;

	rt->_bsz = size;		// init byte tracking
	for (int i = 0; i < (size / 3); i++) {
		_READ(8, &tmpbuf[0], 1, rt);
		_READ(8, &tmpbuf[1], 1, rt);
		_READ(8, &tmpbuf[2], 1, rt);
		rt->palette[i] = (0xff<<24) | tmpbuf[2] << 16 | tmpbuf[1] << 8 | tmpbuf[0];
	}

	san_read_unused(rt);

	return 0;
}

static int handle_XPAL(struct sanrt *rt, uint32_t size)
{
	uint32_t sz2 = size, t32;
	uint8_t t1, t2[3];
	uint16_t t16;
	int i, j;

	rt->_bsz = size;		// init byte tracking
	if (size == 4 || size == 6) {
		for (i = 0; i < 256; i++) {
			for (j = 0; j < 3; j++) {
				t1 = (rt->palette[i] >> (16 - (j * 8))) & 0xff;
				t16 = t1;
				t2[j] = ((t16 * 129) + rt->deltapal[(i * 3) + j]) >> 7; // clip pal ?
			}
			rt->palette[i] = 0xff | (t2[0] << 16) | (t2[1] << 8) | t2 [2];
		}
		sz2 = size;
	} else {
		_READ(LE32, &t32, 1, rt);
		for (i = 0; i < 768; i++) {
			_READ(LE16, &t16, 3, rt);
			rt->deltapal[i] = t16;
		}
		sz2 -= 768 * 2 + 4;
		if (size >= 3844) {
			for (i = 0; i < 256; i++) {
				_READ(8, &t2[0], 1, rt);
				_READ(8, &t2[1], 1, rt);
				_READ(8, &t2[2], 1, rt);
				rt->palette[i] = 0xff000000 | t2[0] << 16 | t2[1] << 8 | t2[2];
			}
			sz2 -= 256 * 3;
		} else {
			memset(rt->palette, 0, 256 * 4);
		}
	}

	san_read_unused(rt);

	return 0;
}

static int handle_IACT(struct sanrt *rt, uint32_t size)
{
	uint16_t v[8];
	uint32_t blocksz = size - 18, len, vv;
	unsigned char *tmpbuf, *iactbuf, *tb2, *dst, *src, *src2, value;

	// size is always a multiple of 2 in the stream, even if the tag reports
	// otherwise.
	if (size & 1)
		size += 1;

	rt->_bsz = size;		// init byte tracking
	_READ(LE16, &v[0], 1, rt);	// code
	_READ(LE16, &v[1], 1, rt);	// flags
	_READ(LE16, &v[2], 1, rt);	// unk
	_READ(LE16, &v[3], 1, rt);	// uid
	_READ(LE16, &v[4], 1, rt);	// trkid
	_READ(LE16, &v[5], 1, rt);	// index
	_READ(LE16, &v[6], 1, rt);	// num frames
	_READ(LE32, &vv, 1, rt);	// size2

	//printf("IACT: size %d code %d flags %d unk %d uid %d trk %d idx %d frames %d size2 %d\n", size, v[0], v[1], v[2], v[3], v[4], v[5], v[6], vv);

	tmpbuf = malloc(blocksz);
	if (!tmpbuf)
		return 41;
	src = tmpbuf;
	if (readX(rt, src, blocksz))
		return 42;
	iactbuf = rt->iactbuf;

	// algorithm taken from scummvm/engines/scumm/smush/smush_player.cpp::handleIACT()
	while (blocksz > 0) {
		if (rt->iactpos >= 2) {
			len = be16_to_cpu((*(uint16_t *)rt->iactbuf));
			len = len + 2 - rt->iactpos;
			if (len > blocksz) {
				memcpy(iactbuf + rt->iactpos, src, blocksz);
				rt->iactpos += blocksz;
				blocksz = 0; 
			} else {
				tb2 = malloc(4096);
				if (!tb2)
					return 43;
				memset(tb2, 0, 4096);
					
				memcpy(iactbuf + rt->iactpos, src, len);
				dst = tb2;
				src2 = iactbuf;
				src2 += rt->iactpos;
				uint8_t v1 = *src2++;
				uint8_t v2 = v1 >> 4;
				uint32_t count = 1024;
				v1 &= 0x0f;
				do {
					value = *(src2++);
					if (value == 0x80) {
						*dst++ = *src2++;
						*dst++ = *src2++;
					} else {
						int16_t v16 = (int8_t)value << v2;
						*dst++ = v16 >> 8;
						*dst++ = v16 & 0xff;
					}
					value = *(src2++);
					if (value == 0x80) {
						*dst++ = *src2++;
						*dst++ = *src2++;
					} else {
						int16_t v16 = (int8_t)value << v1;
						*dst++ = v16 >> 8;
						*dst++ = v16 & 0xff;
					}
				} while (--count);

				// queue audio block
				// this function will take "tb2" pointer and free it when its done.
				(void)rt->queue_audio(rt, tb2, dst - tb2);
				blocksz -= len;
				src += len;
				rt->iactpos = 0;
			}
		} else {
			if (rt->iactpos == 0 && blocksz > 1) {
				*iactbuf = *src++;
				rt->iactpos++;
				blocksz--;
			}
			*(iactbuf + rt->iactpos) = *src++;
			blocksz--;
			rt->iactpos++;
		}
	}
	free(tmpbuf);

	san_read_unused(rt);

	return 0;
}

// subtitles
static int handle_TRES(struct sanrt *rt, uint32_t size)
{
	uint32_t v;
	uint16_t px, py, l, t, w, h, f, strid;

	rt->_bsz = size;		// init byte tracking
	_READ(LE16, &px, 1, rt);
	_READ(LE16, &py, 1, rt);
	_READ(LE16, &f, 1, rt);
	_READ(LE16, &l, 1, rt);
	_READ(LE16, &t, 1, rt);
	_READ(LE16, &w, 1, rt);
	_READ(LE16, &h, 1, rt);
	_READ(LE16, &strid, 1, rt); // dummy
	_READ(LE16, &strid, 1, rt); // real strid

	printf("TRES: sz %u px %u py %u flags %u left %d top %d w %d h %d strid %d\n", size, px, py, f, l, t, w, h, strid);

	san_read_unused(rt);

	return 0;
}

static int handle_STOR(struct sanrt *rt, uint32_t size)
{
	rt->_bsz = size;		// init byte tracking
	rt->to_store = 1;
	san_read_unused(rt);
	return 0;
}

static int handle_FTCH(struct sanrt *rt, uint32_t size)
{
	rt->_bsz = size;		// init byte tracking
	memcpy(rt->buf0, rt->buf3, rt->fbsize);
	san_read_unused(rt);
	return 0;
}

static int handle_FRME(struct sanrt *rt, uint32_t size)
{
	uint32_t cid, csz, framebytes;
	uint8_t v;
	int ret;

	framebytes = size;	// byte tracking
	ret = 0;
	while ((framebytes > 3) && (ret == 0)) {
		ret = readtag(rt, &cid, &csz);
		if (ret)
			return 201;
		if (csz > framebytes)
			return 202;
		switch (cid)
		{
		case NPAL: ret = handle_NPAL(rt, csz); break;
		case FOBJ: ret = handle_FOBJ(rt, csz); break;
		case IACT: ret = handle_IACT(rt, csz); break;
		case TRES: ret = handle_TRES(rt, csz); break;
		case STOR: ret = handle_STOR(rt, csz); break;
		case FTCH: ret = handle_FTCH(rt, csz); break;
		case XPAL: ret = handle_XPAL(rt, csz); break;
		default:
			printf("UNK2: %08x %c%c%c%c %d\n", cid, (cid>>24)%0xff, (cid>>16)&0xff,(cid>>8)&0xff,cid&0xff,csz);
			ret = 61;
			break;
		}
		if (csz & 1)
			csz += 1;
		framebytes -= csz + 8;
	}

	if (framebytes < 4 && ret == 0) {
		// OK Case: most bytes consumed, no errors.
		if (rt->to_store) {
			memcpy(rt->buf3, rt->buf0, rt->fbsize);
		}

		// copy rt->buf0 to output
		ret = rt->queue_video(rt, rt->buf0, rt->w * rt->h * 1);

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
		while (framebytes--)
			read8(rt, &v);

		rt->to_store = 0;
		rt->currframe++;
	}

	return ret;
}

static int handle_AHDR(struct sanrt *rt, uint32_t size)
{
	uint16_t v;
	uint32_t v2;
	int left, ret;
	char tmpbuf[128];

	ret = 0;

	if (size < 768 + 6)
		return 1;		// too small

	rt->_bsz = size;		// init byte tracking
	_READ(8, &tmpbuf[0], 1, rt);
	_READ(8, &tmpbuf[1], 1, rt);
	rt->version = tmpbuf[0] | (tmpbuf[1] << 8);
	_READ(LE16, &(rt->FRMEcnt), 2, rt);
	if (readX(rt, &v, 2))  /* dummy data */
		return 3;

	for (int i = 0; i < 256;  i++) {
		_READ(8, &tmpbuf[0], 4, rt);
		_READ(8, &tmpbuf[1], 4, rt);
		_READ(8, &tmpbuf[2], 4, rt);
		rt->palette[i] = 0xff000000 | tmpbuf[0] << 16 | tmpbuf[1] << 8 | tmpbuf[2];
	}

	if (rt->_bsz < 20) {
		ret = 2;
	} else {
		_READ(LE32, &rt->framerate, 5, rt);
		_READ(LE32, &rt->maxframe, 5, rt);
		_READ(LE32, &rt->samplerate, 5, rt);
		_READ(LE32, &v2, 5, rt);	// unk1
		_READ(LE32, &v2, 5, rt);	// unk2
		ret = 0;
	}

	san_read_unused(rt);

	return ret;
}

int san_one_frame(struct sanrt *rt)
{
	uint32_t cid, csz;
	int ret;

	if (!rt->io)
		return 1;

	if (rt->currframe >= rt->FRMEcnt)
		return 500;

	ret = readtag(rt, &cid, &csz);
	if (ret)
		return 500;	// probably EOF, we're done

	switch (cid) {
		case FRME: ret = handle_FRME(rt, csz); break;
		default:
			printf("UNK1: %08x %c%c%c%c\n", cid, (cid>>24), (cid>>16), (cid>>8), cid&0xff);
			ret = 108;
	}

	return ret;
}

int san_init(struct sanrt **out)
{
	struct sanrt *rt;

	rt = malloc(sizeof(struct sanrt));
	if (!rt)
		return 1;
	memset(rt, 0, sizeof(struct sanrt));

	c47_make_glyphs(rt->glyph4x4[0], glyph4_x, glyph4_y, 4);
	c47_make_glyphs(rt->glyph8x8[0], glyph8_x, glyph8_y, 8);
	*out = rt;

	return 0;
}

int san_open(struct sanrt *rt, struct sanio *io)
{
	int ret, ok;
	uint32_t cid;
	uint32_t csz;
	int have_anim = 0;
	int have_ahdr = 0;

	if (!io)
		return 1;
	rt->io = io;

	while (1) {
		ok = readtag(rt, &cid, &csz);
		if (ok != 0) {
			ret = 2;
			break;
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

	if (!have_ahdr)
		return ret;

	return handle_AHDR(rt, csz);
}
