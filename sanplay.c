/*
 * SDL3-based SAN video player.
 *
 * (c) 2024-2026 Manuel Lauss <manuel.lauss@gmail.com>
 */

#include <stdio.h>
#include "sandec.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_audio.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#define FOPEN(a, b)	gzopen(a, b)
#define FCLOSE(a)	gzclose(a)
#define FREAD(a, b, c)	gzread(a, b, c)
#define IOHDL		gzFile
#else
#define FOPEN(a, b)	fopen(a, b)
#define FCLOSE(a)	fclose(a)
#define FREAD(a, b, c)	fread(b, 1, c, a)
#define IOHDL		FILE*
#endif


struct playpriv {
	IOHDL fhdl;
	SDL_Renderer *ren;
	SDL_Window *win;
	SDL_Surface *lastimg;
	SDL_Surface *newimg;
	SDL_AudioStream *as;

	uint16_t pxw;
	uint16_t pxh;
	uint16_t winw;
	uint16_t winh;

	uint32_t vbufsize;
	uint64_t next_disp_us;
	uint64_t ptick;
	unsigned char *vbuf;
	uint16_t subid;
	int err;
	int fullscreen;
	int nextmult;
	int prevmult;
	int sm;
	int texsmooth;
	int paused;
	int animv1full;
	int autopause;
};

static const SDL_ScaleMode smodes[2] = { SDL_SCALEMODE_NEAREST, SDL_SCALEMODE_LINEAR };

/* this can be called multiple times per "sandec_decode_next_frame()",
 * so buffer needs to be dynamically expanded. */
static void queue_audio(void *ctx, unsigned char *adata, uint32_t size)
{
	struct playpriv *p = (struct playpriv *)ctx;
	if (p->err || p->sm == 2 || !p->as)
		return;

	SDL_PutAudioStreamData(p->as, adata, size);
}

/* this is called once per "sandec_decode_next_frame()" */
static void queue_video(void *ctx, unsigned char *vdata, uint32_t size,
			uint16_t w, uint16_t h, uint16_t pitch,
			uint32_t *imgpal, uint16_t subid, uint32_t frame_duration_us)
{
	struct playpriv *p = (struct playpriv *)ctx;
	SDL_Palette *pal;
	SDL_Surface *sur;
	int ret;

	if (p->err || p->sm == 2)
		return;

	if (p->newimg) {
		p->err = 1105;
		return;
	}
	if ((w == 384) && (h == 242) && (p->animv1full == 0)) {
		p->pxw = 320;
		p->pxh = 200;
	} else {
		p->pxw = w;
		p->pxh = h;
	}

	sur = SDL_CreateSurfaceFrom(p->pxw, p->pxh, imgpal ? SDL_PIXELFORMAT_INDEX8 : SDL_PIXELFORMAT_RGB565,
				    vdata, pitch);
	if (!sur) {
		p->err = 1106;
		return;
	}

	if (imgpal) {
		pal = SDL_CreatePalette(256);
		if (!pal) {
			SDL_DestroySurface(sur);
			p->err = 1107;
			return;
		}
		memcpy(pal->colors, imgpal, 256 * sizeof(uint32_t));
		ret = SDL_SetSurfacePalette(sur, pal);
		if (!ret) {
			SDL_DestroySurface(sur);
			p->err = 1108;
			return;
		}
	}
	p->newimg = sur;
	p->vbufsize = size;
	p->subid = subid;
	p->err = 0;
	p->next_disp_us += frame_duration_us;
}

/* render the current surface to a window */
static int render_frame(struct playpriv *p)
{
	SDL_Texture *tex;
	int ret, nw, nh;

	if (p->err)
		return p->err;

	if (p->sm == 2)
		return 0;

	if (p->pxw < 1 || p->pxh < 1) {
		p->err = 1100;
		goto out;
	}

	if (!p->win) {
		ret = SDL_CreateWindowAndRenderer("SAN/ANIM Player", p->pxw, p->pxh, SDL_WINDOW_RESIZABLE, &p->win, &p->ren);
		if (!ret) {
			p->win = NULL;
			p->ren = NULL;
			p->err = 1101;
			goto out;
		}
		SDL_SetRenderDrawColor(p->ren, 0, 0, 0, SDL_ALPHA_OPAQUE);
		SDL_SetRenderLogicalPresentation(p->ren, p->pxw, p->pxh, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		p->winw = p->pxw;
		p->winh = p->pxh;
		if (p->nextmult == 0)
			p->nextmult = 1;
	}

	if (p->winw != p->pxw || p->winh != p->pxh) {
		SDL_SetWindowSize(p->win, p->pxw, p->pxh);
		p->winw = p->pxw;
		p->winh = p->pxh;
		if (p->nextmult == 0)
			p->nextmult = p->fullscreen ? 0 : p->prevmult;
	}

	if (p->nextmult < 0) {
		p->nextmult = 0;
		if (!p->fullscreen) {
			p->fullscreen = 1;
		} else {
			p->fullscreen = 0;
			p->nextmult = p->prevmult;
		}
	}

	if (p->nextmult > 0) {
		p->prevmult = p->nextmult;
		nw = p->pxw * p->nextmult;
		nh = p->pxh * p->nextmult;
		p->nextmult = 0;
		if (p->fullscreen) {
			SDL_SetWindowFullscreen(p->win, 0);
			p->fullscreen = 0;
			/* SetWindowFullscreen() will trigger an event when done */
			return 0;
		}

		if ((p->winw != nw) || (p->winh != nh)) {
			SDL_SetWindowSize(p->win, nw, nh);
			p->winw = nw;
			p->winh = nh;
		}
	}

	if (!p->ren || !p->lastimg) {
		p->err = 1102;
		goto out;
	}

	tex = SDL_CreateTextureFromSurface(p->ren, p->lastimg);
	if (!tex) {
		p->err = 1103;
		goto out;
	}

	SDL_SetTextureScaleMode(tex, smodes[p->texsmooth]);
	ret = SDL_RenderTexture(p->ren, tex, NULL, NULL);
	SDL_DestroyTexture(tex);
	if (!ret) {
		p->err = 1104;
		goto out;
	}
	SDL_RenderPresent(p->ren);
	if (p->autopause) {
		p->paused = 1;	
		p->ptick = SDL_GetTicks();
	}
	if (p->as && (p->paused == 0))
		SDL_ResumeAudioStreamDevice(p->as);

out:
	return p->err;
}

static int do_img_flip(struct playpriv *p)
{
	/* frame timer expired but no new image was posted */
	if (!p->newimg)
		return 0;

	if (p->lastimg) {
		SDL_DestroySurface(p->lastimg);
		p->lastimg = NULL;
	}
	p->lastimg = p->newimg;
	p->newimg = NULL;
	return render_frame(p);
}

static void exit_sdl(struct playpriv *p)
{
	if (p->as)
		SDL_DestroyAudioStream(p->as);
	if (p->ren)
		SDL_DestroyRenderer(p->ren);
	if (p->win)
		SDL_DestroyWindow(p->win);
	SDL_Quit();
}

static int init_sdl(struct playpriv *p)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *as;

	if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1000;

	SDL_SetHint(SDL_HINT_APP_NAME, "SAN/ANIM Player");
	p->texsmooth = 1;
	p->prevmult = 1;

	spec.freq = 22050;
	spec.format = SDL_AUDIO_S16LE;
	spec.channels = 2;
	as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	p->as = as;
	if (as)
		SDL_PauseAudioStreamDevice(as);

	return 0;
}

static int sio_read(void *ctx, void *dst, uint32_t size)
{
	struct playpriv *p = (struct playpriv *)ctx;
	return FREAD(p->fhdl, dst, size) == size;
}

int main(int a, char **argv)
{
	int ret, speedmode, fc, running, parserdone, i, verbose;
	int sdl_inited;
	uint64_t t1, t2, ren, dec;
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
	sio.flags = 0;

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
			}
			continue;
		} else {
			fn = *argv;
		}

		pp.fhdl = FOPEN(fn, "rb");
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

		pp.autopause = 0;
		if (speedmode == 3) {
			speedmode = 0;
			pp.autopause = 1;
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
		pp.paused = 0;
		pp.ptick = 0;
		parserdone = 0;
		ren = 0;
		dec = 0;

		do {
			ret = sandec_decode_next_frame(sanctx);
		} while (ret == SANDEC_OK && speedmode == 2);

		pp.next_disp_us = SDL_GetTicks() * 1000;
		while (running && ret == SANDEC_OK) {
			while (0 != SDL_PollEvent(&e) && running) {
				if (e.type == SDL_EVENT_QUIT)
					running = 0;
				else if ((e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) ||
					 (e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)) {
					/* act on the fullscreen state change on next frame */
					pp.nextmult = -1;
					ret = render_frame(&pp);
					if (ret)
						break;
				} else if (e.type == SDL_EVENT_KEY_DOWN) {
					SDL_KeyboardEvent *ke = (SDL_KeyboardEvent *)&e;
					if (!ke->down || ke->repeat != 0)
						break;

					if (ke->scancode == SDL_SCANCODE_SPACE && speedmode < 1) {
						if (pp.paused && pp.autopause) {
							pp.paused = 0;
							pp.autopause = 0;
						} else
							pp.paused ^= 1;

						if (!pp.paused) {
							pp.next_disp_us += (SDL_GetTicks() - pp.ptick) * 1000;
							SDL_ResumeAudioStreamDevice(pp.as);
						} else {
							SDL_PauseAudioStreamDevice(pp.as);
							pp.ptick = SDL_GetTicks();
						}
					} else if (ke->scancode == SDL_SCANCODE_N) {
						running = 0;	/* end current video */
						if (pp.as)
							SDL_ClearAudioStream(pp.as);
					} else if (ke->scancode == SDL_SCANCODE_PERIOD) {
						pp.autopause = 1;
						if (pp.paused) {
							pp.paused = 0;
							pp.next_disp_us += (SDL_GetTicks() - pp.ptick) * 1000;
							SDL_ResumeAudioStreamDevice(pp.as);
						}
					} else if (ke->scancode == SDL_SCANCODE_Q) {
						running = 0;
						if (pp.as)
							SDL_ClearAudioStream(pp.as);
						a = 1;		/* no more files to play */
					} else if (ke->scancode == SDL_SCANCODE_F) {
						/* queue fullscreen state change with SDL. Per SDL Documentation,
						 * this may be denied by the windowing system, otherwise a
						 * window state change event will be triggered.
						 */
						SDL_SetWindowFullscreen(pp.win, !pp.fullscreen);
					} else if ((ke->scancode >= SDL_SCANCODE_1) &&
						   (ke->scancode <= SDL_SCANCODE_6)) {
						pp.nextmult = ke->scancode - SDL_SCANCODE_1 + 1;
						ret = render_frame(&pp);
						if (ret)
							break;
					} else if (ke->scancode == SDL_SCANCODE_I) {
						sio.flags ^= SANDEC_FLAG_DO_FRAME_INTERPOLATION;
					} else if (ke->scancode == SDL_SCANCODE_L) {
						pp.animv1full ^= 1;
					} else if (ke->scancode == SDL_SCANCODE_S) {
						pp.texsmooth ^= 1;
						ret = render_frame(&pp);
						if (ret)
							break;
					}
				}
			}

			if (!pp.paused && ret == SANDEC_OK) {

				if (parserdone) {
					if (speedmode) {
						if (pp.as)
							SDL_ClearAudioStream(pp.as);
						running = 0;
					} else if (!pp.as || (0 == SDL_GetAudioStreamQueued(pp.as))) {
						running = 0;
					} else {
						continue;
					}
				}

				t1 = SDL_GetTicks();
				delt = pp.next_disp_us - (ren * 1000) - (t1 * 1000);
				if (running && ((delt <= 0) || speedmode == 1)) {
					ret = do_img_flip(&pp);
					if (ret)
						goto err;
					t2 = SDL_GetTicks();
					ren = (t2 - t1);

					t1 = SDL_GetTicks();
					ret = sandec_decode_next_frame(sanctx);
					t2 = SDL_GetTicks();
					dec = (t2 - t1);
err:
					if (ret == SANDEC_DONE) {
						parserdone = 1;
						SDL_FlushAudioStream(pp.as);
						ret = 0;
					} else if (ret != 0) {
						running = 0;
					}

					if (running && verbose) {
						printf("\33[2K\r%4u/%4u  %lu ms/%lu ms I:%d S:%d P:%d  R:%d", sandec_get_currframe(sanctx), fc, ren, dec, !!(sio.flags & SANDEC_FLAG_DO_FRAME_INTERPOLATION), pp.texsmooth, pp.autopause, ret);
						fflush(stdout);
					}

				} else if (delt > 5000) {
					SDL_Delay(5);
				}
			}
		}

		FCLOSE(pp.fhdl);

		/* let remaining audio finish playback: mainly for .SAD audio files */
		if (ret == SANDEC_DONE && pp.as && speedmode < 2) {
			SDL_ResumeAudioStreamDevice(pp.as);
			SDL_FlushAudioStream(pp.as);
			while (0 != SDL_GetAudioStreamQueued(pp.as))
				SDL_Delay(10);
		}

		if (verbose || ret > SANDEC_OK)
			printf("\n%u/%u  %d\n", sandec_get_currframe(sanctx), fc, ret);
	}


	sandec_exit(&sanctx);
	if (sdl_inited)
		exit_sdl(&pp);
	return ret;
}
