/*
 * SAN ANIMv2 movie decoder, specifically tailored for LucasArts "Outlaws".
 *
 * How to use:
 *
 * int my_queue_audio(void *avctx, char *audio...)
 * {  // can be called more than once per frame decoding
 *   return 0; // 0 == data successfully copied, other = error
 * }
 *
 * int my_queue_videoframe(void *avctx, char *image...)
 * {  // called only once per frame decoding.
 *  return 0;  // 0 == data successfully copied, other = error
 * }
 *
 * int my_data_read(void *ioctx, char *destbuf, int amount)
 * {	// decoder does only linear forward reads, no seeks.
 *	if (destbuf) // destbuf CAN BE NULL!
 * 		return read(ioctx, destbuf, amount)
 * 	else
 * 		return lseek(ioctx, amount);
 * }
 *
 * void *sancontext;
 * struct sanio myio;
 * struct my_avcontext_t my_avctx;
 *
 * int ret = sandec_init(&sancontxt);
 * if (ret != 0) { // error no memory };
 *
 * int fd = open("/path/to/OP_CR.SAN", O_RDONLY);
 * if (fd < 0) { // error file not found };
 * myio.ioctx = &fd;
 * myio.ioread = my_data_read;
 * myio.avctx = my_avctx,
 * myio.queue_audio = my_queue_audio;
 * myio.queue_video = my_queue_video;
 *
 * int ret = sandec_open(sancontext, &myio);
 * if (ret) { // error, code says where it occured }
 *
 * while (ret == 0) {
 *  ret = sandec_decode_next_frame(sancontext);
 *  if (ret != SANDEC_OK) { // error or done; cant decode any more. }
 *  else { render_video_and_audio(my_avctx); }
 * }
 * if (ret != SANDEC_DONE) { // report error code please }
 * sandec_exit(&sancontext);
 * close(fd);
 *
 * NOTES:
 * - The decoder only does linear forward reads. Once data has been read, it
 *    it will not be requested again.
 * - sanio.ioread() can be called with a NULL destination pointer!
 * - sanio.queue_audio() can be called multiple times per frame decoding call.
 * - sanio.queue_video() is only called ONCE per frame decoding call.
 */
#ifndef _SANDEC_H_
#define _SANDEC_H_

#include <inttypes.h>
#include <sys/types.h>

/* status codes */
#define SANDEC_OK	0
#define SANDEC_DONE	-1
/* all other positive values indicate where the error occured */

struct sanio {
	int(*ioread)(void *ioctx, void *dst, uint32_t size);
	int(*queue_video)(void *avctx, unsigned char *vdata, uint32_t size,
			  uint16_t w, uint16_t h, uint32_t *pal, uint16_t subid);
	int(*queue_audio)(void *avctx, unsigned char *adata, uint32_t size);
	void *ioctx;
	void *avctx;
};

/* init SAN context. Call this as step 1. */
int sandec_init(void **sanctx);

/* open/analyze a SAN file, supply with IO structure with at least "read" set. */
int sandec_open(void *sanctx, struct sanio *io);

/* Process one full frame (audio+video).
 * will call the queue_audio() callback multiple times, and queue_video()
 * callback once.
 */
int sandec_decode_next_frame(void *sanctx);

/* destroy all context. call this as last step. */
void sandec_exit(void **sanctx);


int sandec_get_framerate(void *sanctx);
int sandec_get_framecount(void *sanctx);
int sandec_get_currframe(void *sanctx);
int sandec_get_samplerate(void *sanctx);
int sandec_get_version(void *sanctx);

#endif
