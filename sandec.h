/*
 * Decoder library for LucasArts SMUSH/SAN videos.
 *
 * How to use:
 *
 * fetch data callback:  destbuf is always valid, amount is always 1 or more.
 * Return 0 if the requested amount of data was put in the buffer,
 *  otherwise return an error.
 *
 * int my_data_read(void *userctx, char *destbuf, int amount)
 * {    // return 0 if all data was read, others on errors.
 *	return (read(userctx->fhandle, destbuf, amount) != amount);
 * }
 *
 *
 * - Set up decoder context and handle frames:
 *
 * void *sancontext;
 * struct sanio myio;
 * struct my_avcontext_t my_avctx;
 * struct sanframe framedat;
 *
 * int ret = sandec_init(&sancontxt);
 * if (ret != 0) { // error no memory };
 *
 * int fd = open("/path/to/OP_CR.SAN", O_RDONLY);
 * if (fd < 0) { // error file not found };
 * myavctx.fhandle = fd;
 * myio.ioread = my_data_read;
 * myio.userctx = my_avctx;
 *
 * int ret = sandec_open(sancontext, &myio);
 * while (ret == SANDEC_OK) {
 *  ret = sandec_decode_next_frame(sancontext, &framedat);
 *  if (ret != SANDEC_OK)
 * 	break;
 *  render_video_and_audio(my_avctx, &framedat);
 * }
 * close(fd);
 * if (ret != SANDEC_DONE) { // report error code please }
 * sandec_exit(&sancontext);
 *
 * NOTES:
 * - The decoder only does linear forward reads. Once data has been read, it
 *    it will not be requested again.
 */

#ifndef _SANDEC_H_
#define _SANDEC_H_

#include <inttypes.h>

/* status codes */
#define SANDEC_OK	0
#define SANDEC_DONE	-1
/* all other positive values indicate where the error occured */

/* resample all audio to this target rate */
#define SANDEC_AUDIO_SRATE	22050


/* flags */
/* do frame interpolation if possible */
#define SANDEC_FLAG_DO_FRAME_INTERPOLATION	(1 << 0)
/* ignore audio */
#define SANDEC_FLAG_NO_AUDIO			(1 << 1)

struct sanio {
	int(*ioread)(void *userctx, void *dst, uint32_t size);
	void *userctx;
	uint32_t flags;
};

/* A/V Data IO structure.  The pointers point to decoder internal buffers, and are
 * valid only until the next call to sandec_decode_next_frame().
 */
struct sanframe {
	void *ctx;			/* user context, userctx from struct sanio */
	uint32_t frame_duration_us;	/* how long, in microseconds, to display this frame */
	void *vid_data;			/* pointer to memory with planar frame data. */
	uint32_t vid_size;		/* size, in bytes, of data pointed to by @vid_data */
	uint32_t vid_xres;		/* horizontal size, in pixels, of video frame */
	uint32_t vid_yres;		/* vertical size, in pixels, of video frame */
	uint32_t vid_pitch;		/* size, in bytes, of a single line of video frame */
	void *pal;			/* pointer to 32bit ARGB palette data for PAL8 frames */

	void *aud_data;			/* pointer to S16LE audio data */
	uint32_t aud_size;		/* size, in bytes, of memory pointed to by @aud_data */

	uint32_t txtres;		/* id of the text resource for subtitles */
	char *dbg_text;			/* debug text for frame */
};

/* init SAN context. Call this as step 1. */
int sandec_init(void **sanctx);

/* open/analyze a SAN file, supply with IO structure with at least "read" set. */
int sandec_open(void *sanctx, struct sanio *io);

/* Process one full frame (audio+video).
 * will call the queue_audio() callback multiple times, and queue_video()
 * callback once.
 */
int sandec_decode_next_frame(void *sanctx, struct sanframe *data_out);

/* destroy all context. call this as last step. */
void sandec_exit(void **sanctx);

/* get the expected number of frames to render */
int sandec_get_framecount(void *sanctx);

/* get the current rendered frame number */
int sandec_get_currframe(void *sanctx);

/* override parameters; for Mortimer primarily */
int sandec_set_params(void *sanctx, int16_t xres, int16_t yres, int8_t mortimermode);

#endif
