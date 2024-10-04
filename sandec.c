/*
 * SAN ANIM file decoder for Outlaws ".SAN" video files.
 * Outlaws SAN files use SMUSH codec 47 and IACT 22.05kHz 16-bit stereo Audio.
 *
 * Clobbered together from FFmpeg/libavcodec sanm.c decoder and ScummVM
 *  SMUSH player code.
 */

#include <memory.h>
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


static int readX(int h, char *dst, unsigned int cnt)
{
	return read(h, dst, cnt);
}

static uint8_t read8(int h)
{
	uint8_t b;
	read(h, &b, 1);
	return b;
}

static uint16_t read16(int h)
{
	uint16_t s;
	read(h, &s, 2);
	return s;
}

static uint32_t read32(int h)
{
	uint32_t w;
	read(h, &w, 4);
	return w;
}

static uint16_t readLE16(int h)
{
	uint16_t s;
	read(h, &s, 2);
	return le16_to_cpu(s);
}

static uint32_t readLE32(int h)
{
	uint32_t w;
	read(h, &w, 4);
	return le32_to_cpu(w);
}

static uint16_t readBE16(int h)
{
	uint16_t s;
	read(h, &s, 2);
	return be16_to_cpu(s);
}

static uint32_t readBE32(int h)
{
	uint32_t w;
	read(h, &w, 4);
	return be32_to_cpu(w);
}

static uint32_t readBE24(int h)
{
	char b[3];
	uint32_t v;
	read(h, b, 3);
	v = (b[0] << 16) | (b[1]) << 8 | b[2];
	return v;
}

static int readtag(int h, uint32_t *outtag, uint32_t *outsize)
{
	uint32_t v1, v2;
	int ret;

	ret = read(h, &v1, 4);
	if (ret != 4)
		return 1;
	ret = read(h, &v2, 4);
	if (ret != 4)
		return 2;

	v1 = be32_to_cpu(v1);
	v2 = be32_to_cpu(v2);
	*outtag = v1;
	*outsize = v2;
	return 0;
}

// uncompressed full frame
static int codec47_comp0(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h)
{
	int r;

	for (unsigned int i = 0; i < h; i++) {
		r = readX(rt->handle, dst, w);
		if (r != w)
			return 1;
		dst += w;
	}
	return 0;
}

// full frame in half width and height resolution
static int codec47_comp1(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h)
{
	int r;

	for (unsigned int j = 0; j < h; j += 2) {
		for (unsigned int i = 0; i < w; i += 2) {
			dst[i] = dst[i + 1] = dst[w + i] = dst[w + i + 1] = read8(rt->handle);
		}
	}
	return 0;
}

static int codec47_block(struct sanrt *rt, unsigned char *dst, unsigned char *p1, unsigned char *p2, uint16_t w, off_t tblofs, uint16_t size)
{
	uint8_t code, col[2], c;
	uint16_t i, j;
	int8_t *pglyph;

	code = read8(rt->handle);
	if (code >= 0xF8) {
		switch (code) {
		case 0xff:
			if (size == 2) {
				dst[0] = read8(rt->handle);
				dst[1] = read8(rt->handle);
				dst[w] = read8(rt->handle);
				dst[w+1] = read8(rt->handle);
			} else {
				size /= 2;
				codec47_block(rt, dst, p1, p2, w, tblofs, size);
				codec47_block(rt, dst + size, p1 + size, p2 + size, w, tblofs, size);
				dst += (size * w);
				p1 += (size * w);
				p2 += (size * w);
				codec47_block(rt, dst, p1, p2, w, tblofs, size);
				codec47_block(rt, dst + size, p1 + size, p2 + size, w, tblofs, size);
			}
			break;
		case 0xfe:
			c = read8(rt->handle);
			for (i = 0; i < size; i++)
				memset(dst + (i * w), c, size);
			break;
		case 0xfd:
			code = read8(rt->handle);
			col[0] = read8(rt->handle);
			col[1] = read8(rt->handle);
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
			off_t savepos = lseek(rt->handle, 0, SEEK_CUR);
			lseek(rt->handle, tblofs + (code & 7), SEEK_SET);
			c = read8(rt->handle);
			lseek(rt->handle, savepos, SEEK_SET);
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

static int codec47_comp2(struct sanrt *rt, unsigned char *dst, uint16_t w, uint16_t h, uint32_t seq, off_t tblofs)
{
	unsigned int i, j;
	unsigned char *b1, *b2;

	if (seq != (rt->lastseq+1))
		return 0;

	b1 = rt->buf1;
	b2 = rt->buf2;

	for (j = 0; j < h; j += 8) {
		for (i = 0; i < w; i += 8) {
			codec47_block(rt, dst + i, b1 + i, b2 + i, w, tblofs + 8, 8);
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
		opc = read8(rt->handle);
		rlen = (opc >> 1) + 1;
		if (opc & 1) {
			col = read8(rt->handle);
			memset(dst, col, rlen);
		} else {
			readX(rt->handle, dst, rlen);
		}
		dst += rlen;
		left -= rlen;
	}
	return 0;
}

static int codec47(struct sanrt *rt, uint32_t size, uint16_t w, uint16_t h, uint16_t top, uint16_t left)
{
	off_t tblofs = lseek(rt->handle, 0, SEEK_CUR);
	uint16_t seq = readLE16(rt->handle);
	uint8_t comp = read8(rt->handle);
	uint8_t newrot = read8(rt->handle);
	uint8_t skip = read8(rt->handle);
	unsigned char *dst;
	int ret;

	// skip 9 bytes
	(void)read8(rt->handle);
	(void)read32(rt->handle);
	(void)read32(rt->handle);

	uint32_t decsize = readLE32(rt->handle);
	// skip 8 bytes
	(void)read32(rt->handle);
	(void)read32(rt->handle);

	//printf("C47  seq %4u comp %u newrot %u skip %u decsize %u\n", seq, comp, newrot, skip, decsize);
	if (seq == 0) {
		rt->lastseq = (uint32_t)-1;
		memset(rt->buf1, 0, rt->fbsize);
		memset(rt->buf2, 0, rt->fbsize);
	}
	if (skip & 1) {
		lseek(rt->handle, 0x8080, SEEK_CUR);
	}

	dst = rt->buf0 + left + (top * w);
	switch (comp) {
	case 0:	ret = codec47_comp0(rt, dst, w, h); break;
	case 1: ret = codec47_comp1(rt, dst, w, h); break;
	case 2: ret = codec47_comp2(rt, dst, w, h, seq, tblofs); break;
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

	//printf("FOBJ: new buffer %ux%u %u\n", w, h, bs);

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
	off_t off = lseek(rt->handle, 0, SEEK_CUR);
	int ret;

	if (size < 12)
		return 71;

	codec = readLE16(rt->handle);
	left = readLE16(rt->handle);
	top = readLE16(rt->handle);
	w = readLE16(rt->handle);
	h = readLE16(rt->handle);
	(void)readLE32(rt->handle);	// dummy 4 bytes

	//printf("FOBJ size %d %dx%d at %dx%d, codec %d\n", size, w, h, left, top, codec);
	ret = 0;
	if ((rt->w < (left + w)) || (rt->h < (top + h))) {
		ret = fobj_alloc_buffers(rt, _max(rt->w, left + w), _max(rt->h, top + h), 1);
	}
	if (ret != 0)
		goto out;
	if (codec =! 47) {
		ret = 72;
		goto out;
	}
	ret = codec47(rt, size - 14, w, h, top, left);

out:
	lseek(rt->handle, off + size, SEEK_SET);
	return ret;
}

static int handle_NPAL(struct sanrt *rt, uint32_t size)
{
	char tmpbuf[4];
	int j;

	//printf("NPAL size %d\n", size);

	for (int i = 0; i < (size / 3); i++) {
		rt->palette[i] = 0xff000000 | readBE24(rt->handle);
	}
	j = size & 3;
	if (j)
		read(rt->handle, tmpbuf, j);

	rt->newpal = 1;
	return 0;
}

static int handle_XPAL(struct sanrt *rt, uint32_t size)
{
	int i, j;
	uint8_t t1, t2[3];
	uint16_t t;
	uint32_t sz2 = size;

	//printf("XPAL: %u\n", size);

	if (size == 4 || size == 6) {
		for (i = 0; i < 256; i++) {
			for (j = 0; j < 3; j++) {
				t1 = (rt->palette[i] >> (16 - (j * 8))) & 0xff;
				t = t1;
				t2[j] = ((t * 129) + rt->deltapal[(i * 3) + j]) >> 7; // clip pal ?
			}
			rt->palette[i] = 0xff | (t2[0] << 16) | (t2[1] << 8) | t2 [2];
		}
		sz2 = size;
	} else {
		(void)read32(rt->handle);
		for (i = 0; i < 768; i++) {
			rt->deltapal[i] = readLE16(rt->handle);
		}
		sz2 -= 768 * 2 + 4;
		if (size >= 3844) {
			for (i = 0; i < 256; i++) {
				rt->palette[i] = 0xff000000 | readBE24(rt->handle);
			}
			sz2 -= 256 * 3;
		} else {
			memset(rt->palette, 0, 256 * 4);
		}
	}
	for (i = 0; i < sz2; i++)
		(void)read8(rt->handle);
	rt->newpal = 1;
	return 0;
}

static int handle_IACT(struct sanrt *rt, uint32_t size)
{
	uint16_t v[8];
	uint32_t blocksz = size - 18, len, vv;
	unsigned char *tmpbuf, *iactbuf, *tb2, *dst, *src, *src2, value;

	v[0] = readLE16(rt->handle); // code
	v[1] = readLE16(rt->handle); // flags
	v[2] = readLE16(rt->handle); // unk
	v[3] = readLE16(rt->handle); // uid
	v[4] = readLE16(rt->handle); // trkid
	v[5] = readLE16(rt->handle); // index
	v[6] = readLE16(rt->handle); // num frame
	vv = readLE32(rt->handle); // size

	//printf("IACT: size %d code %d flags %d unk %d uid %d trk %d idx %d frames %d size2 %d\n", size, v[0], v[1], v[2], v[3], v[4], v[5], v[6], vv);

	tmpbuf = malloc(blocksz);
	if (!tmpbuf)
		return 41;
	src = tmpbuf;
	readX(rt->handle, src, blocksz);
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
					return 42;
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

	if (size & 1)
		(void)read8(rt->handle);

	return 0;
}

// subtitles
static int handle_TRES(struct sanrt *rt, uint32_t size)
{
	uint32_t v;
	uint16_t px, py, l, t, w, h, f, strid;

	px = readLE16(rt->handle);
	py = readLE16(rt->handle);
	f = readLE16(rt->handle);
	l = readLE16(rt->handle);
	t = readLE16(rt->handle);
	w = readLE16(rt->handle);
	h = readLE16(rt->handle);
	(void)readLE16(rt->handle);
	strid = readLE16(rt->handle);
	printf("TRES: sz %u px %u py %u flags %u left %d top %d w %d h %d strid %d\n", size, px, py, f, l, t, w, h, strid);

	return 0;
}

static int handle_STOR(struct sanrt *rt, uint32_t size)
{
	uint32_t s;

	rt->to_store = 1;

	s = readLE32(rt->handle);
	printf("STOR: sz %u param %08x\n", size, s);

	return 0;
}

static int handle_FTCH(struct sanrt *rt, uint32_t size)
{
	uint32_t v1;
	uint16_t v2;

	memcpy(rt->buf0, rt->buf3, rt->fbsize);
	if (size == 6) {
		v2 = readLE16(rt->handle);
		v1 = readLE32(rt->handle);
		printf("FTCH: sz %d v1 %08x v2 %04x\n", size, v1, v2);
	} else {
		printf("FTCH: sz %d", size);
		lseek(rt->handle, size, SEEK_CUR);
	}

	return 0;
}

static int handle_FRME(struct sanrt *rt, uint32_t size)
{
	int ret;
	uint32_t cid, csz, framebytes;

	//printf("FRME: frame %3d size %d\n", rt->currframe, size);
	framebytes = size;
	ret = 0;
	while (framebytes && (ret == 0)) {
		ret = readtag(rt->handle, &cid, &csz);
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
			lseek(rt->handle, csz, SEEK_CUR);
			ret = 61;
			break;
		}
		if (csz & 1)
			csz += 1;
		framebytes -= csz + 8;
	}
	if (framebytes < 4 && ret == 0) {
		if (rt->to_store) {
			memcpy(rt->buf3, rt->buf0, rt->fbsize);
		}
	}
	// copy rt->buf0 to output
	ret = rt->queue_video(rt, rt->buf0, rt->w * rt->h * 1, rt->newpal);
	rt->newpal = 0;

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

	rt->to_store = 0;
	rt->currframe++;

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

	tmpbuf[0] = read8(rt->handle);
	tmpbuf[1] = read8(rt->handle);
	rt->version = tmpbuf[0] | (tmpbuf[1] << 8);
	rt->FRMEcnt = readLE16(rt->handle);
	/* dummy data */
	read(rt->handle, &v, 2);

	//printf("AHDR size %d version %d FRMEs %d ", size, rt->version, rt->FRMEcnt);

	rt->newpal = 1;
	for (int i = 0; i < 256;  i++)
		rt->palette[i] = 0xff000000 | readBE24(rt->handle);

	left = size - (768 + 6);
	if (left < 20) {
		ret = 2;
	} else {
		rt->framerate = readLE32(rt->handle);
		rt->maxframe = readLE32(rt->handle);
		rt->samplerate = readLE32(rt->handle);
		(void)readLE32(rt->handle);	// unk1
		(void)readLE32(rt->handle);	// unk2
		//printf("framerate %d maxframe %d samplerate %d", rt->framerate, rt->maxframe, rt->samplerate);
		left -= 20;
		ret = 0;
	}
	printf("\n");

	while (left) {
		(void)read8(rt->handle);
		left--;
	}

	return ret;
}

int san_one_frame(struct sanrt *rt)
{
	uint32_t cid, csz;
	int ret;

	if (rt->currframe >= rt->FRMEcnt)
		return 500;

	ret = readtag(rt->handle, &cid, &csz);
	if (ret == 1)
		return 500;	// probably EOF, we're done

		switch (cid) {
			case FRME: ret = handle_FRME(rt, csz); break;
			default:
				printf("UNK1: %08x %c%c%c%c\n", cid, (cid>>24), (cid>>16), (cid>>8), cid&0xff);
				ret = 108;
		}

		return ret;
}

int san_open(int handle, struct sanrt** out)
{
	int ret, ok;
	char buf[128];
	struct sanrt *rt;
	uint32_t cid;
	uint32_t csz;
	int have_anim = 0;
	int have_ahdr = 0;

	while (1) {
		ok = readtag(handle, &cid, &csz);
		if (ok != 0) {
			ret = 31;
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

	rt = malloc(sizeof(struct sanrt));
	if (!rt)
		return 32;
	memset(rt, 0, sizeof(struct sanrt));
	rt->handle = handle;
	c47_make_glyphs(rt->glyph4x4[0], glyph4_x, glyph4_y, 4);
	c47_make_glyphs(rt->glyph8x8[0], glyph8_x, glyph8_y, 8);
	ret = handle_AHDR(rt, csz);
	if (ret) {
		free(rt);
		*out = NULL;
	} else {
		*out = rt;
	}

	return ret;
}
