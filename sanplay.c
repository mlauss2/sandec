/*
 * SDL2-based SAN video player.
 *
 * (c) 2024 Manuel Lauss <manuel.lauss@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include "sandec.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <SDL2/SDL_audio.h>

struct sdlpriv {
	SDL_Renderer *ren;
	SDL_Window *win;
	SDL_AudioDeviceID aud;

	uint32_t abufsize;
	uint32_t abufptr;
	unsigned char *abuf;

	uint16_t w;
	uint16_t h;
	uint32_t vbufsize;
	unsigned char *vbuf;
	uint32_t *pal;
	uint16_t subid;
	int err;
};

/* this can be called multiple times per "sandec_decode_next_frame()",
 * so buffer needs to be dynamically expanded. */
static void queue_audio(void *avctx, unsigned char *adata, uint32_t size)
{
	struct sdlpriv *p = (struct sdlpriv *)avctx;
	if (!p || p->err)
		return;

	while ((p->abufptr + size) > p->abufsize) {
		uint32_t newsize = p->abufsize + 16384;
		p->abuf = realloc(p->abuf, newsize);
		if (!p->abuf) {
			p->err = 2;
			return;
		}
		p->abufsize = newsize;
	}
	memcpy(p->abuf + p->abufptr, adata, size);
	p->abufptr += size;

	/* alternatively, the buffer could also just be queued to SDL immediately,
	 * as SDL does the same thing the code above tries to do.
	SDL_QueueAudio(p->aud, adata, size);
	 */
}

/* this is called once per "sandec_decode_next_frame()" */
static void queue_video(void *avctx, unsigned char *vdata, uint32_t size,
		       uint16_t w, uint16_t h, uint32_t *imgpal, uint16_t subid)
{
	struct sdlpriv *p = (struct sdlpriv *)avctx;
	if (!p || p->err)
		return;

	/* we borrow the buffer and palette. these pointers are valid until
	 * the next invocation of san_decode_next_frame().
	 */
	p->vbuf = vdata;
	p->vbufsize = size;
	p->pal = imgpal;
	p->w = w;
	p->h = h;
	p->subid = subid;
}

static int render_frame(struct sdlpriv *p)
{
	SDL_Surface *sur;
	SDL_Texture *tex;
	SDL_Palette *pal;
	SDL_Rect sr;
	int ret;

	if (p->err)
		return p->err;

	sr.x = sr.y = 0;
	sr.w = p->w;
	sr.h = p->h;

	sur = SDL_CreateRGBSurfaceWithFormatFrom(p->vbuf, sr.w, sr.h, 8, sr.w, SDL_PIXELFORMAT_INDEX8);
	if (!sur) {
		printf("ERR: %s\n", SDL_GetError());
		return 1001;
	}

	pal = SDL_AllocPalette(256);
	if (!pal)
		return 1005;
	memcpy(pal->colors, p->pal, 256 * sizeof(SDL_Color));
	ret = SDL_SetSurfacePalette(sur, pal);
	if (ret) {
		SDL_FreeSurface(sur);
		return 1002;
	}
	tex = SDL_CreateTextureFromSurface(p->ren, sur);
	if (!tex) {
		SDL_FreeSurface(sur);
		return 1003;
	}

	ret = SDL_RenderCopy(p->ren, tex, &sr, &sr);
	if (ret) {
		return 1004;
	}
	SDL_RenderPresent(p->ren);
	SDL_QueueAudio(p->aud, p->abuf, p->abufptr);
	p->abufptr = 0;

	SDL_DestroyTexture(tex);
	SDL_FreeSurface(sur);

	return 0;
}

static int init_sdl_vid(struct sdlpriv *p)
{
	int ret = SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	if (ret)
		return 81;

	SDL_SetHint(SDL_HINT_APP_NAME, "SAN Player");
	SDL_Window *win = SDL_CreateWindow("SAN Player",
					   SDL_WINDOWPOS_CENTERED,
					   SDL_WINDOWPOS_CENTERED,
					   640, 480, SDL_WINDOW_RESIZABLE);
	if (!win)
		return 82;

	p->win = win;

	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
	if (!ren) {
		SDL_DestroyWindow(win);
		return 83;
	}
	p->ren = ren;

	return 0;
}

static void exit_sdl_vid(struct sdlpriv *p)
{
	SDL_DestroyRenderer(p->ren);
	SDL_DestroyWindow(p->win);
	free(p->abuf);
	SDL_Quit();
}

static int init_sdl_aud(struct sdlpriv *p)
{
	SDL_AudioSpec specin, specout;
	SDL_AudioDeviceID ad;

	specin.freq = 22050;
	specin.format = AUDIO_S16;
	specin.channels = 2;
	specin.userdata = p;
	specin.callback = NULL;
	specin.samples = 4096 / 2 / 2;
	ad = SDL_OpenAudioDevice(NULL, 0, &specin, &specout, 0);
	if (!ad)
		return 1;

	p->aud = ad;
	SDL_PauseAudioDevice(ad, 0);
	return 0;
}

static void exit_sdl_aud(struct sdlpriv *p)
{
	SDL_CloseAudioDevice(p->aud);
}

static int init_sdl(struct sdlpriv *p)
{
	int ret;

	memset(p, 0, sizeof(struct sdlpriv));
	ret = init_sdl_vid(p);
	if (ret)
		goto err0;
	ret = init_sdl_aud(p);
	if (ret)
		goto err1;

	return 0;

err1:
	exit_sdl_vid(p);
err0:
	SDL_Quit();
	return ret;
}

static void exit_sdl(struct sdlpriv *p)
{
	exit_sdl_aud(p);
	exit_sdl_vid(p);
	SDL_Quit();
}

static int sio_read(void *ctx, void *dst, uint32_t size)
{
	int hdl = *(int *)ctx;
	return read(hdl, dst, size) == size;
}

int main(int a, char **argv)
{
	int running, paused, parserdone;
	struct sdlpriv sdl;
	struct sanio sio;
	void *sanctx;
	SDL_Event e;
	int fr, h, ret, speedmode, waittick;
	uint64_t t1;

	if (a < 2) {
		printf("arg missing\n");
		return 1;
	}

	speedmode = (a == 3) ? strtol(argv[2], NULL, 10) : 0;

	h = open(argv[1], O_RDONLY);
	if (!h) {
		printf("cannot open\n");
		return 2;
	}

	ret = sandec_init(&sanctx);
	if (ret) {
		printf("SAN init failed: %d\n", ret);
		goto out;
	}

	if (speedmode < 2) {
		ret = init_sdl(&sdl);
		if (ret)
			goto out;
	}
	sio.ioctx = &h;
	sio.ioread = sio_read;
	sio.avctx = (speedmode < 2) ? &sdl : NULL;
	sio.queue_audio = queue_audio;
	sio.queue_video = queue_video;

	ret = sandec_open(sanctx, &sio);
	if (ret) {
		printf("SAN invalid: %d\n", ret);
		goto out;
	}

	fr = sandec_get_framerate(sanctx);
	printf("SAN ver %u fps %u sr %u FRMEs %u\n", sandec_get_version(sanctx),
	       fr, sandec_get_samplerate(sanctx),
	       sandec_get_framecount(sanctx));

	running = 1;
	paused = 0;
	parserdone = 0;
	
	if (!fr)
		fr = 2;
	waittick = 1000 / fr;
	while (running) {
		while (0 != SDL_PollEvent(&e) && running) {
			if (e.type == SDL_QUIT)
				running = 0;
		}

		if (!paused && running) {
			if (!parserdone) {
				t1 = SDL_GetTicks64();
				ret = sandec_decode_next_frame(sanctx);
				if (ret == SANDEC_OK) {
					if (speedmode < 2) {
						ret = render_frame(&sdl);
						SDL_Delay(waittick - (SDL_GetTicks64() - t1));
					} else
						ret = 0;
				} else {
					printf("ret %d at %d\n", ret, sandec_get_currframe(sanctx));
					if (ret == SANDEC_DONE)
						parserdone = 1;
					else
						running = 0;
				}
			}
			if (parserdone) {
				if (speedmode) {
					SDL_ClearQueuedAudio(sdl.aud);
					running = 0;
				} else if (0 == SDL_GetQueuedAudioSize(sdl.aud)) {
					running = 0;
				}
			}
		}
	}

	printf("sanloop exited with code %d, played %d FRMEs\n", ret, sandec_get_currframe(sanctx));

	sandec_exit(&sanctx);
	if (speedmode < 2)
		exit_sdl(&sdl);
out:
	close(h);
	return ret;
}
