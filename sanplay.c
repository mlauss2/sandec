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
	struct sanrt *san;
	SDL_Renderer *ren;
	SDL_Window *win;
	SDL_AudioDeviceID aud;
	SDL_Color col[256];
	void *ptr1;
	void *ptr2;
};

static int queue_audio(struct sanrt *rt, unsigned char *adata, uint32_t size)
{
	struct sdlpriv *p = rt->userdata;
	uint32_t fid = rt->currframe;
	int ret;

	ret = SDL_QueueAudio(p->aud, adata, size);
	free(adata);

	printf("AUDIO: %p : %u at %u Hz  fid %u %d\n", adata, size, rt->samplerate, fid, ret);

	return 0;
}

static int queue_video(struct sanrt *rt, unsigned char *vdata, uint32_t size, int newpal)
{
	struct sdlpriv *p = rt->userdata;
	uint32_t fid = rt->currframe;
	SDL_Surface *sur;
	SDL_Texture *tex;
	SDL_Palette *pal;
	SDL_Rect sr, dr;
	int ret;

	printf("VIDEO: %p %u %ux%x %d fid %u\n", vdata, size, rt->w, rt->h, newpal, fid);

	sr.x = sr.y = 0;
	sr.w = rt->w;
	sr.h = rt->h;

	sur = SDL_CreateRGBSurfaceWithFormatFrom(vdata, rt->w, rt->h, 8, rt->w, SDL_PIXELFORMAT_INDEX8);
	if (!sur) {
		printf("ERR: %s\n", SDL_GetError());
		return 1001;
	}
	if (newpal) {
		for (int i = 0; i < 256; i++) {
			// the layout of the colors in rt->palette is identical
			// to the components in SDL_Color, so do a 32bit copy.
			*(uint32_t *)(&p->col[i]) = *(uint32_t *)&(rt->palette[i]);
/* per-byte version:
			p->col[i].r = (rt->palette[i] >> 0) & 0xff;
			p->col[i].g = (rt->palette[i] >> 8) & 0xff;
			p->col[i].b = (rt->palette[i] >>16) & 0xff;
			p->col[i].a = 0xff;
*/
		}
	}
	pal = SDL_AllocPalette(256);
	if (!pal)
		return 1005;
	memcpy(pal->colors, p->col, 256 * sizeof(SDL_Color));
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
	SDL_DestroyTexture(tex);
	SDL_FreeSurface(sur);

	return 0;
}

static int audiocb(void)
{

}

static int init_sdl_vid(struct sdlpriv *p)
{
	int ret = SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	if (ret)
		return 81;
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
	int h, ret;
	struct sanrt *san;
	struct sanio sio;
	struct sdlpriv sdl;
	SDL_Event e;
	int running, paused, parserdone;
	struct timespec ts;

	if (a < 2) {
		printf("arg missing\n");
		return 1;
	}

	h = open(argv[1], O_RDONLY);
	if (!h) {
		printf("cannot open\n");
		return 2;
	}

	ret = san_init(&san);
	if (ret) {
		printf("SAN init failed: %d\n", ret);
		goto out;
	}

	sio.ctx = &h;
	sio.read = sio_read;

	ret = san_open(san, &sio);
	if (ret) {
		printf("SAN invalid: %d\n", ret);
		goto out;
	}

	printf("SAN ver %u fps %u sr %u FRMEs %u\n", san->version, san->framerate,
	       san->samplerate, san->FRMEcnt);

	ret = init_sdl(&sdl);
	if (ret)
		goto out;

	san->userdata = &sdl;
	san->queue_audio = queue_audio;
	san->queue_video = queue_video;

	running = 1;
	paused = 0;
	parserdone = 0;

	// frame pacing
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000000 / san->framerate;

	while (running) {
		while (0 != SDL_PollEvent(&e) && running) {
			if (e.type == SDL_QUIT)
				running = 0;
		}

		if (!paused) {
			if (!parserdone) {
				ret = san_one_frame(san);
				if (ret	== 500)
					parserdone = 1;
				else if (ret != 0)
					running = 0;
			}
			if (!SDL_GetQueuedAudioSize(sdl.aud))
				running = 0;
		}
		nanosleep(&ts, NULL);
	}


	if (ret != 500) {
		printf("sanloop exited with error %d\n", ret);
	} else {
		printf("sanloop exit OK\n");
	}

	free(san);
out1:
	exit_sdl(&sdl);
out:
	close(h);
	return ret;
}
