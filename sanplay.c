/*
 * SDL2-based SAN video player.
 *
 * (c) 2024 Manuel Lauss <manuel.lauss@gmail.com>
 */

#define __USE_MISC
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
	SDL_Color col[256];

	uint32_t abufsize;
	uint32_t abufptr;
	unsigned char *abuf;

	uint16_t w;
	uint16_t h;
	uint32_t vbufsize;
	unsigned char *vbuf;
	uint32_t pal[256];
};

// this can be called multiple times per "san_one_frame()", so buffer needs
// to be dynamically expanded.
static int queue_audio(void *avctx, unsigned char *adata, uint32_t size)
{
	struct sdlpriv *p = (struct sdlpriv *)avctx;
	int ret;

	while ((p->abufptr + size) > p->abufsize) {
		uint32_t newsize = p->abufsize + 16384;
		p->abuf = realloc(p->abuf, newsize);
		if (!p->abuf)
			return 1010;
		p->abufsize = newsize;
	}
	memcpy(p->abuf + p->abufptr, adata, size);
	p->abufptr += size;

	return 0;
}

// this is called once per "san_one_frame()"
static int queue_video(void *avctx, unsigned char *vdata, uint32_t size, uint16_t w, uint16_t h, uint32_t *imgpal)
{
	struct sdlpriv *p = (struct sdlpriv *)avctx;

	//printf("VIDEO: %p %u %ux%x fid %u\n", vdata, size, rt->w, rt->h, fid);
	if (size > p->vbufsize) {
		p->vbuf = realloc(p->vbuf, size);
		if (!p->vbuf)
			return 1011;
		p->vbufsize = size;
	}
	memcpy(p->vbuf, vdata, size);
	if (imgpal) {
		memcpy(p->pal, imgpal, 256 * 4);
	}
	p->w = w;
	p->h = h;

	return 0;
}

static int render_frame(struct sdlpriv *p)
{
	SDL_Surface *sur;
	SDL_Texture *tex;
	SDL_Palette *pal;
	SDL_Rect sr, dr;
	int ret;

	sr.x = sr.y = 0;
	sr.w = p->w;
	sr.h = p->h;

	sur = SDL_CreateRGBSurfaceWithFormatFrom(p->vbuf, p->w, p->h, 8, p->w, SDL_PIXELFORMAT_INDEX8);
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
}

static int init_sdl_aud(struct sdlpriv *p)
{
	SDL_AudioSpec specin, specout;
	SDL_AudioDeviceID ad;

	specin.freq = 22050;
	specin.format = AUDIO_S16;
	specin.channels = 2;
	specin.userdata = p;
	specin.callback = 0;	// SDL_QueueAudio() !
	specin.samples = 1024; // 4096 / stereo / 16bit
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
	if (!dst) {
		lseek(hdl, size, SEEK_CUR);
		return size;
	}
	return read(hdl, dst, size);
}

int main(int a, char **argv)
{
	int running, paused, parserdone;
	struct timespec ts;
	struct sdlpriv sdl;
	struct sanio sio;
	void *sanctx;
	SDL_Event e;
	int h, ret;

	if (a < 2) {
		printf("arg missing\n");
		return 1;
	}

	h = open(argv[1], O_RDONLY);
	if (!h) {
		printf("cannot open\n");
		return 2;
	}

	ret = san_init(&sanctx);
	if (ret) {
		printf("SAN init failed: %d\n", ret);
		goto out;
	}

	memset(&sdl, 0, sizeof(sdl));
	ret = init_sdl(&sdl);
	if (ret)
		goto out;

	sio.ioctx = &h;
	sio.read = sio_read;
	sio.avctx = &sdl;
	sio.queue_audio = queue_audio;
	sio.queue_video = queue_video;

	ret = san_open(sanctx, &sio);
	if (ret) {
		printf("SAN invalid: %d\n", ret);
		goto out;
	}

	printf("SAN ver %u fps %u sr %u FRMEs %u\n", san_get_version(sanctx),
	       san_get_framerate(sanctx), san_get_samplerate(sanctx), san_get_framecount(sanctx));



	running = 1;
	paused = 0;
	parserdone = 0;

	// frame pacing
	ts.tv_sec = 0;
	ts.tv_nsec = 970000000 / san_get_framerate(sanctx);

	while (running) {
		while (0 != SDL_PollEvent(&e) && running) {
			if (e.type == SDL_QUIT)
				running = 0;
		}

		if (!paused && running) {
			if (!parserdone) {
				ret = san_one_frame(sanctx);
				if (ret == 0) {
					ret = render_frame(&sdl);
					nanosleep(&ts, NULL);
				} else {
					printf("ret %d at %d\n", ret, san_get_currframe(sanctx));
					if (ret < 0)
						parserdone = 1;
					else
						running = 0;
				}

			}
			if (parserdone && !SDL_GetQueuedAudioSize(sdl.aud))
				running = 0;
		}
	}

	free(sdl.abuf);
	free(sdl.vbuf);

	printf("sanloop exited with code %d, played %d FRMEs\n", ret, san_get_currframe(sanctx));

	free(sanctx);
out1:
	exit_sdl(&sdl);
out:
	close(h);
	return ret;
}
