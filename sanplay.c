/*
 * SDL2-based SAN video player.
 *
 * (c) 2024 Manuel Lauss <manuel.lauss@gmail.com>
 */

#include <stdio.h>
#include "sandec.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <SDL2/SDL_audio.h>

struct sdlpriv {
	SDL_Renderer *ren;
	SDL_Window *win;
	SDL_AudioDeviceID aud;

	uint16_t pxw;
	uint16_t pxh;
	uint16_t winw;
	uint16_t winh;

	uint32_t vbufsize;
	uint32_t frame_duration;
	unsigned char *vbuf;
	uint16_t subid;
	int err;
	int fullscreen;
	int nextmult;
};

/* this can be called multiple times per "sandec_decode_next_frame()",
 * so buffer needs to be dynamically expanded. */
static void queue_audio(void *avctx, unsigned char *adata, uint32_t size)
{
	struct sdlpriv *p = (struct sdlpriv *)avctx;
	if (!p || p->err)
		return;

	SDL_QueueAudio(p->aud, adata, size);
}

/* this is called once per "sandec_decode_next_frame()" */
static void queue_video(void *avctx, unsigned char *vdata, uint32_t size,
			uint16_t w, uint16_t h, uint32_t *imgpal, uint16_t subid,
			uint32_t frame_duration_us)
{
	struct sdlpriv *p = (struct sdlpriv *)avctx;
	SDL_Palette *pal;
	SDL_Surface *sur;
	SDL_Texture *tex;
	int ret, nw, nh;

	if (!p || p->err)
		return;

	if (!p->win) {
		ret = SDL_CreateWindowAndRenderer(w, h, SDL_WINDOW_RESIZABLE, &p->win, &p->ren);
		if (ret) {
			p->win = NULL;
			p->ren = NULL;
			p->err = 1100;
			return;
		}
		SDL_SetWindowTitle(p->win, "SAN/ANIM Player");
		p->winw = w;
		p->winh = h;
	}

	if (p->winw < w || p->winh < h) {
		SDL_SetWindowSize(p->win, w, h);
		p->winw = w;
		p->winh = h;
	}

	if (p->nextmult) {
		nw = w * p->nextmult;
		nh = h * p->nextmult;
		p->nextmult = 0;
		if ((p->winw != nw) || (p->winh != nh)) {
			SDL_SetWindowSize(p->win, nw, nh);
			p->winw = nw;
			p->winh = nh;
		}
	}

	sur = SDL_CreateRGBSurfaceWithFormatFrom(vdata, w, h, 8, w, SDL_PIXELFORMAT_INDEX8);
	if (!sur) {
		p->err = 1101;
		return;
	}

	pal = SDL_AllocPalette(256);
	if (!pal) {
		SDL_FreeSurface(sur);
		p->err = 1102;
		return;
	}
	memcpy(pal->colors, imgpal, 256 * sizeof(uint32_t));
	ret = SDL_SetSurfacePalette(sur, pal);
	if (ret) {
		SDL_FreeSurface(sur);
		p->err = 1103;
		return;
	}

	tex = SDL_CreateTextureFromSurface(p->ren, sur);
	if (!tex) {
		p->err = 1104;
		return;
	}
	ret = SDL_RenderCopy(p->ren, tex, NULL, NULL);
	if (ret) {
		p->err = 1003;
		return;
	}
	SDL_DestroyTexture(tex);
	SDL_FreeSurface(sur);
	p->vbufsize = size;
	p->pxw = w;
	p->pxh = h;
	p->subid = subid;
	p->err = 0;
	p->frame_duration = frame_duration_us;
}

static int render_frame(struct sdlpriv *p)
{
	if (p->err)
		return p->err;

	SDL_RenderPresent(p->ren);

	return 0;
}

static void exit_sdl(struct sdlpriv *p)
{
	if (p->aud)
		SDL_CloseAudioDevice(p->aud);
	if (p->ren)
		SDL_DestroyRenderer(p->ren);
	if (p->win)
		SDL_DestroyWindow(p->win);
	SDL_Quit();
}

static int init_sdl(struct sdlpriv *p)
{
	SDL_AudioSpec specin, specout;
	SDL_AudioDeviceID ad;
	int ret;

	memset(p, 0, sizeof(struct sdlpriv));

	ret = SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	if (ret)
		return 1081;

	SDL_SetHint(SDL_HINT_APP_NAME, "SAN/ANIM Player");
	specin.freq = 22050;
	specin.format = AUDIO_S16;
	specin.channels = 2;
	specin.userdata = p;
	specin.callback = NULL;
	specin.samples = 4096 / 2 / 2;

	ad = SDL_OpenAudioDevice(NULL, 0, &specin, &specout, 0);
	if (!ad) {
		ret = 1082;
		goto err;
	}

	p->aud = ad;
	SDL_PauseAudioDevice(ad, 0);

	return 0;

err:
	exit_sdl(p);
	return ret;
}

static int sio_read(void *ctx, void *dst, uint32_t size)
{
	FILE *hdl = *(FILE **)ctx;
	return fread(dst, 1, size, hdl) == size;
}

int main(int a, char **argv)
{
	int ret, speedmode, dtick, fc, running, paused, parserdone;
	uint64_t t1, t2, ren, dec;
	struct sdlpriv sdl;
	struct sanio sio;
	void *sanctx;
	SDL_Event e;
	FILE *h;

	if (a < 2) {
		printf("usage: %s <file.san/.anm> [speedmode]\n speedmode ", argv[0]);
		printf("1: ignore frametime, 2 don't render audio/video\n");
		return 1;
	}

	speedmode = (a == 3) ? strtol(argv[2], NULL, 10) : 0;

	h = fopen(argv[1], "r");
	if (!h) {
		printf("cannot open file %s\n", argv[1]);
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

	fc = sandec_get_framecount(sanctx);

	running = 1;
	paused = 0;
	parserdone = 0;
	dtick = 0;
	ren = 0;
	dec = 0;

	ret = sandec_decode_next_frame(sanctx);
	while (running && ret == 0) {
		while (0 != SDL_PollEvent(&e) && running) {
			if (e.type == SDL_QUIT)
				running = 0;
			else if (e.type == SDL_KEYDOWN) {
					SDL_KeyboardEvent *ke = (SDL_KeyboardEvent *)&e;
					if (ke->state != SDL_PRESSED || ke->repeat != 0)
						break;

					if (ke->keysym.scancode == SDL_SCANCODE_SPACE && speedmode < 1) {
						paused ^= 1;
						SDL_PauseAudioDevice(sdl.aud, paused);
					} else if (ke->keysym.scancode == SDL_SCANCODE_Q) {
						running = 0;
					} else if (ke->keysym.scancode == SDL_SCANCODE_F) {
						sdl.fullscreen ^= 1;
					} else if ((ke->keysym.scancode >= SDL_SCANCODE_1) &&
					    (ke->keysym.scancode <= SDL_SCANCODE_6)) {
						sdl.nextmult = ke->keysym.scancode - SDL_SCANCODE_1 + 1;
					}
			}
		}

		if (!paused) {

			if (parserdone) {
				if (speedmode) {
					SDL_ClearQueuedAudio(sdl.aud);
					running = 0;
				} else if (0 == SDL_GetQueuedAudioSize(sdl.aud)) {
					running = 0;
				} else {
					continue;
				}
			}

			if (running) {
				if (speedmode < 2) {
					t1 = SDL_GetTicks64();
					ret = render_frame(&sdl);
				 if (ret)
						goto err;
					t2 = SDL_GetTicks64();
					ren = (t2 - t1);
				} else {
					ren = 0;
				}

				t1 = SDL_GetTicks64();
				ret = sandec_decode_next_frame(sanctx);
err:
				if (ret == SANDEC_DONE) {
					parserdone = 1;
					ret = 0;
				} else if (ret != 0) {
					running = 0;
				}

				t2 = SDL_GetTicks64();
				dec = (t2 - t1);

				if (running && speedmode < 2) {
					t1 = SDL_GetTicks64();
					printf("\r                           ");
					printf("\r%u/%u  %lu ms/%lu ms %d", sandec_get_currframe(sanctx), fc, ren, dec, ret);
					fflush(stdout);
					t2 = SDL_GetTicks64();
					dtick = (sdl.frame_duration / 1000) - ren - dec - (t2 - t1);
					if (speedmode < 1 && dtick > 0)
						SDL_Delay(dtick);
				}
			}
		}
	}

	printf("\n%u/%u  %d\n", sandec_get_currframe(sanctx), fc, ret);

	sandec_exit(&sanctx);
	if (speedmode < 2)
		exit_sdl(&sdl);
out:
	fclose(h);
	return ret;
}
