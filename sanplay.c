/*
 * SDL2-based SAN video player.
 *
 * (c) 2024-2025 Manuel Lauss <manuel.lauss@gmail.com>
 */

#include <stdio.h>
#include "sandec.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_video.h>
#include <SDL2/SDL_audio.h>

struct playpriv {
	FILE *fhdl;
	SDL_Renderer *ren;
	SDL_Window *win;
	SDL_AudioDeviceID aud;
	SDL_Rect dr;
	SDL_Rect *drp;

	uint16_t pxw;
	uint16_t pxh;
	uint16_t winw;
	uint16_t winh;

	uint32_t vbufsize;
	uint64_t next_disp_us;
	unsigned char *vbuf;
	uint16_t subid;
	int err;
	int fullscreen;
	int rcc;
	int nextmult;
	int prevmult;
	int sm;
	int texsmooth;
};

/* this can be called multiple times per "sandec_decode_next_frame()",
 * so buffer needs to be dynamically expanded. */
static void queue_audio(void *ctx, unsigned char *adata, uint32_t size)
{
	struct playpriv *p = (struct playpriv *)ctx;
	if (p->err || p->sm == 2 || !p->aud)
		return;

	SDL_QueueAudio(p->aud, adata, size);
}

/* this is called once per "sandec_decode_next_frame()" */
static void queue_video(void *ctx, unsigned char *vdata, uint32_t size,
			uint16_t w, uint16_t h, uint16_t pitch,
			uint32_t *imgpal, uint16_t subid, uint32_t frame_duration_us)
{
	struct playpriv *p = (struct playpriv *)ctx;
	SDL_Palette *pal;
	SDL_Surface *sur;
	SDL_Texture *tex;
	int ret, nw, nh, fw, fh, xd, yd;

	if (p->err || p->sm == 2)
		return;

	if (!p->win) {
		ret = SDL_CreateWindowAndRenderer(w, h, SDL_WINDOW_RESIZABLE, &p->win, &p->ren);
		if (ret) {
			p->win = NULL;
			p->ren = NULL;
			p->err = 1100;
			return;
		}
		SDL_SetRenderDrawColor(p->ren, 0, 0, 0, 0);
		SDL_SetWindowTitle(p->win, "SAN/ANIM Player");
		p->winw = w;
		p->winh = h;
		if (p->nextmult == 0)
			p->nextmult = 1;
	}

	if (p->winw < w || p->winh < h) {
		SDL_SetWindowSize(p->win, w, h);
		p->winw = w;
		p->winh = h;
		if (p->nextmult == 0)
			p->nextmult = p->fullscreen ? -1 : p->prevmult;
	}

	if (p->nextmult < 0) {
		p->nextmult = 0;
		if (!p->fullscreen) {
			SDL_SetWindowFullscreen(p->win, SDL_WINDOW_FULLSCREEN_DESKTOP);
			p->fullscreen = 1;
			p->rcc = 3;
			SDL_GetWindowSize(p->win, &fw, &fh);
			xd = (fw * 1024) / w;
			yd = (fh * 1024) / h;
			if (xd == yd) {
				p->drp = NULL;
			} else  if (xd >= yd) {
				p->dr.h = fh;
				p->dr.y = 0;
				p->dr.w = (w * fh) / h;
				p->dr.x = (fw - p->dr.w) / 2;
				p->drp = &p->dr;
			} else {
				p->dr.w = fw;
				p->dr.x = 0;
				p->dr.h = (h * fw) / w;
				p->dr.y = (fh - p->dr.h) / 2;
				p->drp = &p->dr;
			}

		} else {
			SDL_SetWindowFullscreen(p->win, 0);
			p->nextmult = p->prevmult;
			p->fullscreen = 0;
		}
	}

	if (p->nextmult > 0) {
		p->prevmult = p->nextmult;
		nw = w * p->nextmult;
		nh = h * p->nextmult;
		p->nextmult = 0;
		if (p->fullscreen) {
			SDL_SetWindowFullscreen(p->win, 0);
			p->fullscreen = 0;
		}

		if ((p->winw != nw) || (p->winh != nh)) {
			SDL_SetWindowSize(p->win, nw, nh);
			p->winw = nw;
			p->winh = nh;
		}
		p->drp = NULL;
	}

	sur = SDL_CreateRGBSurfaceWithFormatFrom(vdata, w, h, 8, pitch,
			 imgpal ? SDL_PIXELFORMAT_INDEX8 : SDL_PIXELFORMAT_RGB565);
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

	/* clear all render buffers, otherwise there will be a copy of the last
	 * windowed image at the top left.  Unfortunately no way to clear them
	 * all with a single call, so have to flip a few times to get them all.
	 */
	if (p->fullscreen && p->rcc--)
		SDL_RenderClear(p->ren);

	ret = SDL_RenderCopy(p->ren, tex, NULL, p->drp);
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
	p->next_disp_us += frame_duration_us;
}

static int render_frame(struct playpriv *p)
{
	if (p->err)
		return p->err;

	SDL_RenderPresent(p->ren);

	return 0;
}

static void exit_sdl(struct playpriv *p)
{
	if (p->aud)
		SDL_CloseAudioDevice(p->aud);
	if (p->ren)
		SDL_DestroyRenderer(p->ren);
	if (p->win)
		SDL_DestroyWindow(p->win);
	SDL_Quit();
}

static int init_sdl(struct playpriv *p)
{
	SDL_AudioSpec specin, specout;
	SDL_AudioDeviceID ad;
	int ret;

	ret = SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	if (ret)
		return 1081;

	SDL_SetHint(SDL_HINT_APP_NAME, "SAN/ANIM Player");
	p->texsmooth = 2;
	p->prevmult = 1;
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
	specin.freq = 22050;
	specin.format = AUDIO_S16;
	specin.channels = 2;
	specin.userdata = p;
	specin.callback = NULL;
	specin.samples = 4096 / 2 / 2;

	ad = SDL_OpenAudioDevice(NULL, 0, &specin, &specout, 0);
	p->aud = ad;
	if (ad)
		SDL_PauseAudioDevice(ad, 0);

	return 0;
}

static int sio_read(void *ctx, void *dst, uint32_t size)
{
	struct playpriv *p = (struct playpriv *)ctx;
	return fread(dst, 1, size, p->fhdl) == size;
}

int main(int a, char **argv)
{
	int ret, speedmode, fc, running, paused, parserdone, autopause, i, verbose;
	int sdl_inited;
	uint64_t t1, t2, ren, dec, ptick;
	struct playpriv pp;
	struct sanio sio;
	int64_t delt;
	void *sanctx;
	SDL_Event e;
	char *fn, *c;

	verbose = 0;
	sdl_inited = 0;

	if (a < 2) {
		printf("usage: %s [-f] [-v] [-s] [-[0..3]] <file.san/.anm> [file2.san] [file3.san] ...\n", argv[0]);
		printf(" -f  start fullscreen\n -v be verbose\n -s no audio\n -0..3 speedmode\n");
		return 1001;
	}

	ret = sandec_init(&sanctx);
	if (ret) {
		printf("SAN init failed: %d\n", ret);
		return 1002;
	}

	memset(&sio, 0, sizeof(struct sanio));
	memset(&pp, 0, sizeof(struct playpriv));

	sio.ioread = sio_read;
	sio.userctx = &pp;
	sio.queue_audio = queue_audio;
	sio.queue_video = queue_video;
	sio.flags = SANDEC_FLAG_DO_FRAME_INTERPOLATION;

	speedmode = 0;

	while (--a) {
		argv++;
		c = *argv;
		i = strlen(c);
		if (i < 2)
			continue;

		if (c[0] == '-') {
			while (i--) {
				switch(c[1]) {
				case '0': case '1': case '2': case '3': speedmode = c[1] - '0'; break;
				case 'f': pp.nextmult = -1; break;
				case 'v': verbose++; break;
				case 's': sio.flags |= SANDEC_FLAG_NO_AUDIO; break;
				}
				c++;
				if (speedmode == 2)
					sio.flags &= ~SANDEC_FLAG_DO_FRAME_INTERPOLATION;
			}
			continue;
		} else {
			fn = *argv;
		}

		pp.fhdl = fopen(fn, "r");
		if (!pp.fhdl) {
			if (verbose)
				printf("cannot open file %s\n", fn);
			continue;
		}


		if (!sdl_inited && (speedmode < 2)) {
			ret = init_sdl(&pp);
			if (ret)
				break;
			else
				sdl_inited = 1;
		}

		autopause = 0;
		if (speedmode == 3) {
			speedmode = 0;
			autopause = 1;
		}

		pp.sm = speedmode;

		ret = sandec_open(sanctx, &sio);
		if (ret) {
			if (verbose)
				printf("SAN invalid: %d\n", ret);
			continue;
		}

		fc = sandec_get_framecount(sanctx);
		pp.err = 0;
		running = 1;
		paused = 0;
		ptick = 0;
		parserdone = 0;
		ren = 0;
		dec = 0;

		do {
			ret = sandec_decode_next_frame(sanctx);
		} while (ret == SANDEC_OK && speedmode == 2);

		pp.next_disp_us = SDL_GetTicks64() * 1000;
		while (running && ret == SANDEC_OK) {
			while (0 != SDL_PollEvent(&e) && running) {
				if (e.type == SDL_QUIT)
					running = 0;
				else if (e.type == SDL_KEYDOWN) {
					SDL_KeyboardEvent *ke = (SDL_KeyboardEvent *)&e;
					if (ke->state != SDL_PRESSED || ke->repeat != 0)
						break;

					if (ke->keysym.scancode == SDL_SCANCODE_SPACE && speedmode < 1) {
						paused ^= 1;
						if (!paused)
							pp.next_disp_us += (SDL_GetTicks64() - ptick) * 1000;
						if (pp.aud)
							SDL_PauseAudioDevice(pp.aud, paused);
						if (paused)
							ptick = SDL_GetTicks64();
					} else if (ke->keysym.scancode == SDL_SCANCODE_N) {
						running = 0;	/* end current video */
						if (pp.aud)
							SDL_ClearQueuedAudio(pp.aud);
					} else if (ke->keysym.scancode == SDL_SCANCODE_P) {
						autopause ^= 1;
					} else if (ke->keysym.scancode == SDL_SCANCODE_Q) {
						running = 0;
						if (pp.aud)
							SDL_ClearQueuedAudio(pp.aud);
						a = 1;		/* no more files to play */
					} else if (ke->keysym.scancode == SDL_SCANCODE_F) {
						pp.nextmult = -1;
					} else if ((ke->keysym.scancode >= SDL_SCANCODE_1) &&
						   (ke->keysym.scancode <= SDL_SCANCODE_6)) {
						pp.nextmult = ke->keysym.scancode - SDL_SCANCODE_1 + 1;
					} else if (ke->keysym.scancode == SDL_SCANCODE_I) {
						sio.flags ^= SANDEC_FLAG_DO_FRAME_INTERPOLATION;
					} else if (ke->keysym.scancode == SDL_SCANCODE_S) {
						pp.texsmooth += 1;
						if (pp.texsmooth >= 3)
							pp.texsmooth = 0;
						if (pp.texsmooth == 0)
							SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
						else if (pp.texsmooth == 1)
							SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
						else
							SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
					}
				}
			}

			if (!paused) {

				if (parserdone) {
					if (speedmode) {
						if (pp.aud)
							SDL_ClearQueuedAudio(pp.aud);
						running = 0;
					} else if (!pp.aud || (0 == SDL_GetQueuedAudioSize(pp.aud))) {
						running = 0;
					} else {
						continue;
					}
				}

				t1 = SDL_GetTicks64();
				delt = pp.next_disp_us - (ren * 1000) - (t1 * 1000);
				if (running && ((delt <= 0) || speedmode == 1)) {
					ret = render_frame(&pp);
					if (ret)
						goto err;
					t2 = SDL_GetTicks64();
					ren = (t2 - t1);

					t1 = SDL_GetTicks64();
					ret = sandec_decode_next_frame(sanctx);
					t2 = SDL_GetTicks64();
					dec = (t2 - t1);
err:
					if (ret == SANDEC_DONE) {
						parserdone = 1;
						ret = 0;
					} else if (ret != 0) {
						running = 0;
					}

					if (running && verbose) {
						printf("\33[2K\r%4u/%4u  %lu ms/%lu ms I:%d S:%d P:%d  R:%d", sandec_get_currframe(sanctx), fc, ren, dec, !!(sio.flags & SANDEC_FLAG_DO_FRAME_INTERPOLATION), pp.texsmooth, autopause, ret);
						fflush(stdout);
					}
					if (autopause) {
						paused = 1;
						SDL_PauseAudioDevice(pp.aud, paused);
						ptick = SDL_GetTicks64();
					}
				} else if (delt > 5000) {
					SDL_Delay(5);
				}
			}
		}

		fclose(pp.fhdl);
		if (verbose)
			printf("\n%u/%u  %d\n", sandec_get_currframe(sanctx), fc, ret);
	}


	sandec_exit(&sanctx);
	if (sdl_inited)
		exit_sdl(&pp);
	return ret;
}
