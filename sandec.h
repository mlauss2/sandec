#ifndef _SANDEC_H_
#define _SANDEC_H_

#include <inttypes.h>
#include <sys/types.h>

#define NGLYPHS 256

struct sanio {
	int(*read)(void *ioctx, void *dst, uint32_t size);
	int(*queue_video)(void *avctx, unsigned char *vdata, uint32_t size, uint16_t w, uint16_t h, uint32_t *pal);
	int(*queue_audio)(void *avctx, unsigned char *adata, uint32_t size);
	void *ioctx;
	void *avctx;
};

// init SAN context. Call this as step 1.
int san_init(void **sanctx);

// open/analyze a SAN file, supply with IO structure with at least "read" set.
int san_open(void *sanctx, struct sanio *io);

/* Process one full frame (audio+video).
 * will call the queue_audio() callback multiple times, and queue_video()
 * callback once.
 */
int san_one_frame(void *sanctx);


int san_get_framerate(void *sanctx);
int san_get_framecount(void *sanctx);
int san_get_currframe(void *sanctx);
int san_get_samplerate(void *sanctx);
int san_get_version(void *sanctx);

#endif
