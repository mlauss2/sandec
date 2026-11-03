/*
 * SDL3-based SAN video player.
 *
 * (c) 2024-2026 Manuel Lauss <manuel.lauss@gmail.com>
 */

#include <stdio.h>
#include <string.h>
#include "sandec.h"

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
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
#define IOHDL		FILE *
#endif

struct playpriv {
	IOHDL fhdl;
	SDL_Renderer *ren;
	SDL_Window *win;

	SDL_Surface *surf;
	SDL_Texture *tex;
	int have_newimg;

	SDL_AudioStream *as;

	uint16_t pxw;
	uint16_t pxh;
	uint16_t winw;
	uint16_t winh;
	uint16_t texw;
	uint16_t texh;

	uint32_t vbufsize;
	uint64_t next_disp_us;
	uint64_t ptick_us;
	unsigned char *vbuf;
	uint32_t subid;
	int err;
	int fullscreen;
	int nextmult;
	int prevmult;
	int texsmooth;
	int paused;
	int animv1full;
	int autopause;

	int grid_on;
	int grid_color;
	int grid_corner;	/* 0 = bottom-right (default), 1 = top-left */

	int mouse_x;
	int mouse_y;
	int mouse_valid;

	/* Timer / custom event state */
	uint32_t ev_frame;
	uint32_t ev_drain;
	SDL_TimerID frame_timer;
	SDL_TimerID drain_timer;
	int32_t timer_gen;

	/* App / playlist state */
	int argc;
	char **argv;
	int arg_idx;
	void *sanctx;
	struct sanio sio;
	int speedmode;
	int verbose;
	int sdl_inited;
	int mortimermode;
	int fc;
	int running;
	int parserdone;
	int draining_audio;
	int last_ret;
	uint64_t ren_us;
	uint64_t dec_us;
};

static const SDL_ScaleMode smodes[2] = {
	SDL_SCALEMODE_NEAREST,
	SDL_SCALEMODE_LINEAR
};

static const SDL_Color grid_colors[5] = {
	{ 0,   0,   0,   SDL_ALPHA_OPAQUE },	/* black */
	{ 255, 255, 255, SDL_ALPHA_OPAQUE },	/* white */
	{ 0,   255, 0,   SDL_ALPHA_OPAQUE },	/* green */
	{ 255, 0,   0,   SDL_ALPHA_OPAQUE },	/* red */
	{ 0,   0,   255, SDL_ALPHA_OPAQUE },	/* blue */
};

static inline uint64_t now_us(void)
{
	return SDL_GetTicksNS() / 1000ULL;
}

static uint64_t SDLCALL frame_timer_cb(void *userdata, SDL_TimerID id, uint64_t interval)
{
	struct playpriv *p = (struct playpriv *)userdata;
	SDL_Event ev;

	(void)id;
	(void)interval;

	SDL_zero(ev);
	ev.type = p->ev_frame;
	ev.user.code = p->timer_gen;
	SDL_PushEvent(&ev);

	return 0;
}

static uint32_t SDLCALL drain_timer_cb(void *userdata, SDL_TimerID id, uint32_t interval)
{
	struct playpriv *p = (struct playpriv *)userdata;
	SDL_Event ev;

	(void)id;

	SDL_zero(ev);
	ev.type = p->ev_drain;
	SDL_PushEvent(&ev);

	return interval;
}

static void stop_frame_timer(struct playpriv *p)
{
	if (p->frame_timer) {
		SDL_RemoveTimer(p->frame_timer);
		p->frame_timer = 0;
	}
	p->timer_gen++;
}

static void stop_drain_timer(struct playpriv *p)
{
	if (p->drain_timer) {
		SDL_RemoveTimer(p->drain_timer);
		p->drain_timer = 0;
	}
}

static void start_drain_timer(struct playpriv *p)
{
	if (!p->drain_timer && p->ev_drain)
		p->drain_timer = SDL_AddTimer(10, drain_timer_cb, p);
}

static void schedule_next_frame(struct playpriv *p)
{
	int64_t delt_us;
	SDL_Event ev;

	stop_frame_timer(p);

	if (!p->running || p->paused || p->parserdone || !p->ev_frame)
		return;

	delt_us = p->next_disp_us - p->ren_us - now_us();
	if (delt_us <= 0 || p->speedmode == 1) {
		SDL_zero(ev);
		ev.type = p->ev_frame;
		ev.user.code = p->timer_gen;
		SDL_PushEvent(&ev);
	} else {
		p->frame_timer = SDL_AddTimerNS(delt_us * 1000ULL, frame_timer_cb, p);
	}
}

static void queue_audio(struct playpriv *p, const void *adata, uint32_t size)
{
	if (p->err || !p->as || !adata || !size)
		return;

	SDL_PutAudioStreamData(p->as, adata, (int)size);
}

static void queue_video(struct playpriv *p, const struct sanframe *f)
{
	SDL_Palette *pal;
	uint16_t w, h;
	int i, linesz;

	if (p->err || !f->vid_data || !f->vid_size)
		return;

	if (p->have_newimg) {
		p->err = 1105;
		return;
	}

	w = (uint16_t)f->vid_xres;
	h = (uint16_t)f->vid_yres;

	if (w == 384 && h == 242 && p->animv1full == 0) {
		p->pxw = 320;
		p->pxh = 200;
	} else {
		p->pxw = w;
		p->pxh = h;
	}

	if (!p->surf || p->surf->w != p->pxw || p->surf->h != p->pxh) {
		if (p->surf)
			SDL_DestroySurface(p->surf);
		p->surf = SDL_CreateSurface(p->pxw, p->pxh,
					    f->pal ? SDL_PIXELFORMAT_INDEX8 :
						     SDL_PIXELFORMAT_RGB565);
		if (!p->surf) {
			p->err = 1106;
			return;
		}
	}

	if ((uint32_t)p->surf->pitch == f->vid_pitch) {
		memcpy(p->surf->pixels, f->vid_data, p->pxh * f->vid_pitch);
	} else {
		linesz = p->pxw * (f->pal ? 1 : 2);
		for (i = 0; i < p->pxh; i++) {
			memcpy((uint8_t *)p->surf->pixels + (i * p->surf->pitch),
			       (const uint8_t *)f->vid_data + (i * f->vid_pitch),
			       linesz);
		}
	}

	if (f->pal) {
		pal = SDL_CreatePalette(256);
		if (pal) {
			memcpy(pal->colors, f->pal, 256 * sizeof(uint32_t));
			SDL_SetSurfacePalette(p->surf, pal);
			SDL_DestroyPalette(pal);
		} else {
			p->err = 1107;
			return;
		}
	}

	p->have_newimg = 1;
	p->vbufsize = f->vid_size;
	p->subid = f->txtres;
	p->err = 0;
	p->next_disp_us += f->frame_duration_us;
}

static int decode_next(struct playpriv *p)
{
	struct sanframe f;
	int ret;

	memset(&f, 0, sizeof(f));
	ret = sandec_decode_next_frame(p->sanctx, &f);
	if (ret != SANDEC_OK)
		return ret;

	if (f.aud_data && f.aud_size)
		queue_audio(p, f.aud_data, f.aud_size);
	if (f.vid_data && f.vid_size)
		queue_video(p, &f);

	return p->err ? p->err : SANDEC_OK;
}

/* render a 1-pixel-wide (1 pixel on SDL window that is) grid so I can count
 * source pixels better.
 */
static void render_grid(struct playpriv *p, uint16_t dispw, uint16_t disph)
{
	SDL_FRect dst;
	const SDL_Color *col;
	int dx, dy, dw, dh, x1, y1, pos;
	uint16_t x, y;

	if (!p->grid_on || !p->texw || !p->texh)
		return;

	if (!SDL_GetRenderLogicalPresentationRect(p->ren, &dst))
		return;

	dx = (int)dst.x;
	dy = (int)dst.y;
	dw = (int)dst.w;
	dh = (int)dst.h;

	/* only do it if the window actually is >4x the size of the source image,
	 * otherwise we'd only see the grid.
	 */
	if (dw < p->texw * 2 || dh < p->texh * 2)
		return;

	SDL_SetRenderLogicalPresentation(p->ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
	col = &grid_colors[p->grid_color];
	SDL_SetRenderDrawColor(p->ren, col->r, col->g, col->b, col->a);

	x1 = dx + dw - 1;
	y1 = dy + dh - 1;

	for (x = 0; x < p->texw; x++) {
		if (p->grid_corner == 0) {
			pos = dx + ((x + 1) * dw) / p->texw - 1;
		} else {
			pos = dx + (x * dw) / p->texw;
		}
		SDL_RenderLine(p->ren, pos, dy, pos, y1);
	}

	for (y = 0; y < p->texh; y++) {
		if (p->grid_corner == 0) {
			pos = dy + ((y + 1) * dh) / p->texh - 1;
		} else {
			pos = dy + (y * dh) / p->texh;
		}
		SDL_RenderLine(p->ren, dx, pos, x1, pos);
	}

	SDL_SetRenderDrawColor(p->ren, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_SetRenderLogicalPresentation(p->ren, dispw, disph, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

static int render_frame(struct playpriv *p, int flip)
{
	int ret, nw, nh;
	uint16_t dispw, disph;

	if (p->err)
		return p->err;

	if (!flip && !p->tex)
		return 0;

	dispw = flip ? p->pxw : p->texw;
	disph = flip ? p->pxh : p->texh;

	if (dispw < 1 || disph < 1) {
		p->err = 1100;
		goto out;
	}

	if (!p->win) {
		ret = SDL_CreateWindowAndRenderer("SAN/ANIM Player",
						  dispw, disph,
						  SDL_WINDOW_RESIZABLE,
						  &p->win, &p->ren);
		if (!ret) {
			p->win = NULL;
			p->ren = NULL;
			p->err = 1101;
			goto out;
		}
		SDL_SetRenderDrawColor(p->ren, 0, 0, 0, SDL_ALPHA_OPAQUE);
		SDL_SetRenderLogicalPresentation(p->ren, dispw, disph, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		p->winw = dispw;
		p->winh = disph;
		if (p->nextmult == 0)
			p->nextmult = 1;
	}

	if (flip && (p->winw != dispw || p->winh != disph)) {
		SDL_SetRenderLogicalPresentation(p->ren, dispw, disph, SDL_LOGICAL_PRESENTATION_LETTERBOX);
		SDL_SetWindowSize(p->win, dispw, disph);
		p->winw = dispw;
		p->winh = disph;
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
		nw = dispw * p->nextmult;
		nh = disph * p->nextmult;
		p->nextmult = 0;
		if (p->fullscreen) {
			SDL_SetWindowFullscreen(p->win, 0);
			p->fullscreen = 0;
			/* SetWindowFullscreen() will trigger an event when done */
			return 0;
		}

		if (p->winw != nw || p->winh != nh) {
			SDL_SetWindowSize(p->win, nw, nh);
			p->winw = nw;
			p->winh = nh;
		}
	}

	if (!p->ren) {
		p->err = 1102;
		goto out;
	}

	if (flip && p->have_newimg && p->surf) {
		if (!p->tex || p->texw != p->pxw || p->texh != p->pxh) {
			if (p->tex)
				SDL_DestroyTexture(p->tex);

			p->tex = SDL_CreateTexture(p->ren, p->surf->format,
						   SDL_TEXTUREACCESS_STREAMING,
						   p->pxw, p->pxh);
			if (!p->tex) {
				p->err = 1103;
				goto out;
			}
			p->texw = p->pxw;
			p->texh = p->pxh;
		}

		SDL_UpdateTexture(p->tex, NULL, p->surf->pixels, p->surf->pitch);
		if (p->surf->format == SDL_PIXELFORMAT_INDEX8)
			SDL_SetTexturePalette(p->tex, SDL_GetSurfacePalette(p->surf));

		p->have_newimg = 0;
	}

	if (!p->tex) {
		p->err = 1108;
		goto out;
	}

	SDL_RenderClear(p->ren);
	SDL_SetTextureScaleMode(p->tex, smodes[p->texsmooth]);
	ret = SDL_RenderTexture(p->ren, p->tex, NULL, NULL);
	if (!ret) {
		p->err = 1104;
		goto out;
	}

	render_grid(p, dispw, disph);
	SDL_RenderPresent(p->ren);

	if (flip) {
		if (p->autopause) {
			p->paused = 1;
			p->ptick_us = now_us();
		}
		if (p->as && p->paused == 0)
			SDL_ResumeAudioStreamDevice(p->as);
	}

out:
	return p->err;
}

static int init_sdl(struct playpriv *p)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *as;

	if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		return 1000;

	p->ev_frame = SDL_RegisterEvents(2);
	if (p->ev_frame == 0)
		return 1001;
	p->ev_drain = p->ev_frame + 1;

	SDL_SetHint(SDL_HINT_APP_NAME, "SAN/ANIM Player");
	p->texsmooth = 1;
	p->prevmult = 1;

	spec.freq = SANDEC_AUDIO_SRATE;
	spec.format = SDL_AUDIO_S16LE;
	spec.channels = 2;
	as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
				       &spec, NULL, NULL);
	p->as = as;
	if (as)
		SDL_PauseAudioStreamDevice(as);

	p->sdl_inited = 1;
	return 0;
}

static int sio_read(void *ctx, void *dst, uint32_t size)
{
	struct playpriv *p = (struct playpriv *)ctx;

	return FREAD(p->fhdl, dst, size) != size;
}

static void finish_current_file(struct playpriv *p)
{
	stop_frame_timer(p);
	stop_drain_timer(p);

	if (p->fhdl) {
		FCLOSE(p->fhdl);
		p->fhdl = NULL;
	}

	p->running = 0;

	/* Let remaining audio finish playback: mainly for .SAD audio files */
	if (p->last_ret == SANDEC_DONE && p->as && p->speedmode < 2) {
		SDL_ResumeAudioStreamDevice(p->as);
		SDL_FlushAudioStream(p->as);
		if (SDL_GetAudioStreamQueued(p->as) != 0) {
			p->draining_audio = 1;
			start_drain_timer(p);
			return;
		}
	}

	if (p->verbose || p->last_ret > SANDEC_OK)
		printf("\n%u/%u  %d\n", sandec_get_currframe(p->sanctx),
		       p->fc, p->last_ret);
}

static void speedmode2(struct playpriv *p, const char *fn)
{
	struct sanframe f;
	uint64_t t0, t1, dt_ns, ms_int, ms_frac, fps_x10;
	uint32_t frames;
	int ret;

	t0 = SDL_GetTicksNS();
	do {
		ret = sandec_decode_next_frame(p->sanctx, &f);
	} while (ret == SANDEC_OK);
	t1 = SDL_GetTicksNS();

	FCLOSE(p->fhdl);
	p->fhdl = NULL;
	p->last_ret = ret;

	if (p->verbose || ret > SANDEC_OK) {
		frames = sandec_get_currframe(p->sanctx);
		dt_ns = t1 - t0;
		ms_int = dt_ns / 1000000ULL;
		ms_frac = (dt_ns % 1000000ULL) / 10000ULL;
		fps_x10 = dt_ns ? ((uint64_t)frames * 10000000000ULL) / dt_ns : 0;
		printf("%s: %u/%d frames in %llu.%02llu ms (%llu.%llu fps) ret=%d\n",
		       fn, frames, p->fc, (unsigned long long)ms_int,
		       (unsigned long long)ms_frac,
		       (unsigned long long)(fps_x10 / 10ULL),
		       (unsigned long long)(fps_x10 % 10ULL),
		       ret);
	}
}

static int open_next_file(struct playpriv *p)
{
	int ret, i;
	char *fn, *c;

	while (p->arg_idx < p->argc) {
		c = p->argv[p->arg_idx++];
		i = (int)strlen(c);
		if (i < 2)
			continue;

		if (c[0] == '-') {
			while (i--) {
				switch (c[1]) {
				case '0':
				case '1':
				case '2':
				case '3':
					p->speedmode = c[1] - '0';
					break;
				case 'f':
					p->nextmult = -1;
					break;
				case 'v':
					p->verbose++;
					break;
				case 's':
					p->sio.flags |= SANDEC_FLAG_NO_AUDIO;
					break;
				case 'm':
					p->mortimermode = 1;
					break;
				}
				c++;
			}
			continue;
		}

		fn = c;
		p->fhdl = FOPEN(fn, "rb");
		if (!p->fhdl) {
			if (p->verbose)
				printf("cannot open file %s\n", fn);
			continue;
		}

		if (!p->sdl_inited && p->speedmode < 2) {
			ret = init_sdl(p);
			if (ret) {
				FCLOSE(p->fhdl);
				p->fhdl = NULL;
				return -1;
			}
		}

		p->autopause = 0;
		if (p->speedmode == 3) {
			p->speedmode = 0;
			p->autopause = 1;
		}

		ret = sandec_open(p->sanctx, &p->sio);
		if (ret) {
			if (p->verbose)
				printf("SAN invalid: %d\n", ret);
			FCLOSE(p->fhdl);
			p->fhdl = NULL;
			continue;
		}

		if (p->mortimermode) {
			ret = sandec_set_params(p->sanctx, 768, 484, 1);
			if (ret && p->verbose)
				printf("Mortimer params failed: %d\n", ret);
		}

		p->fc = sandec_get_framecount(p->sanctx);

		if (p->speedmode == 2) {
			speedmode2(p, fn);
			continue;
		}

		p->err = 0;
		p->have_newimg = 0;
		p->running = 1;
		p->paused = 0;
		p->ptick_us = 0;
		p->parserdone = 0;
		p->ren_us = 0;
		p->dec_us = 0;

		ret = decode_next(p);
		p->last_ret = ret;
		p->next_disp_us = now_us();

		if (ret != SANDEC_OK) {
			finish_current_file(p);
			if (p->draining_audio)
				return 1;
			continue;
		}

		schedule_next_frame(p);
		return 1;
	}

	return 0;
}

static void print_status(struct playpriv *p)
{
	int gx;

	if (!p->verbose || !p->running)
		return;

	gx = p->grid_on ? (p->grid_color + 1) : 0;

	printf("\33[2K\r%4u/%4u  %lu ms/%lu ms I:%d S:%d P:%d G:%d/%d R:%d M:%d",
	       sandec_get_currframe(p->sanctx), p->fc,
	       (unsigned long)(p->ren_us / 1000ULL),
	       (unsigned long)(p->dec_us / 1000ULL),
	       !!(p->sio.flags & SANDEC_FLAG_DO_FRAME_INTERPOLATION),
	       p->texsmooth, p->autopause, gx, p->grid_corner,
	       p->last_ret, p->mortimermode);

	if (!p->fullscreen && p->mouse_valid)
		printf(" M:(%03d,%03d)", p->mouse_x, p->mouse_y);

	fflush(stdout);
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
	struct playpriv *p;
	int ret;

	if (argc < 2) {
		printf("usage: %s [-f] [-v] [-s] [-[0..3]] <file.san/.anm> [file2.san] [file3.san] ...\n",
		       argv[0]);
		printf(" -f  start fullscreen\n -v be verbose\n -s no audio\n -0..3 speedmode\n");
		return SDL_APP_FAILURE;
	}

	SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

	p = SDL_calloc(1, sizeof(*p));
	if (!p)
		return SDL_APP_FAILURE;

	ret = sandec_init(&p->sanctx);
	if (ret) {
		printf("SAN init failed: %d\n", ret);
		SDL_free(p);
		return SDL_APP_FAILURE;
	}

	p->argc = argc;
	p->argv = argv;
	p->arg_idx = 1;

	p->sio.ioread = sio_read;
	p->sio.userctx = p;
	p->sio.flags = 0;

	*appstate = p;

	/* Open the first file now so waitevent has an initial event (or exits in -2 mode) */
	ret = open_next_file(p);
	if (ret < 0)
		return SDL_APP_FAILURE;
	if (ret == 0)
		return SDL_APP_SUCCESS;

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
	struct playpriv *p = (struct playpriv *)appstate;
	SDL_KeyboardEvent *ke;
	int ret;

	if (event->type == SDL_EVENT_QUIT)
		return SDL_APP_SUCCESS;

	if (p->ev_drain && event->type == p->ev_drain) {
		if (p->draining_audio) {
			if (!p->as || SDL_GetAudioStreamQueued(p->as) == 0) {
				stop_drain_timer(p);
				p->draining_audio = 0;
				if (p->verbose || p->last_ret > SANDEC_OK)
					printf("\n%u/%u  %d\n",
					       sandec_get_currframe(p->sanctx),
					       p->fc, p->last_ret);
			}
		} else if (p->running && p->parserdone) {
			if (p->speedmode) {
				if (p->as)
					SDL_ClearAudioStream(p->as);
				finish_current_file(p);
			} else if (!p->as || SDL_GetAudioStreamQueued(p->as) == 0) {
				finish_current_file(p);
			}
		} else {
			stop_drain_timer(p);
		}
		return SDL_APP_CONTINUE;
	}

	if (!p->running)
		return SDL_APP_CONTINUE;

	if (p->ev_frame && event->type == p->ev_frame) {
		uint64_t t1, t2;

		if (event->user.code != p->timer_gen || p->paused || p->parserdone)
			return SDL_APP_CONTINUE;

		p->frame_timer = 0;

		t1 = now_us();
		if (!p->have_newimg) {
			if (p->as && !p->paused)
				SDL_ResumeAudioStreamDevice(p->as);
			ret = 0;
		} else {
			ret = render_frame(p, 1);
		}
		if (!ret) {
			t2 = now_us();
			p->ren_us = t2 - t1;

			t1 = now_us();
			ret = decode_next(p);
			t2 = now_us();
			p->dec_us = t2 - t1;
		}

		p->last_ret = ret;
		if (ret == SANDEC_DONE) {
			p->parserdone = 1;
			p->last_ret = 0;
			if (p->speedmode) {
				if (p->as)
					SDL_ClearAudioStream(p->as);
				finish_current_file(p);
				return SDL_APP_CONTINUE;
			}
			if (p->as) {
				SDL_ResumeAudioStreamDevice(p->as);
				SDL_FlushAudioStream(p->as);
			}
			if (!p->as || SDL_GetAudioStreamQueued(p->as) == 0) {
				finish_current_file(p);
				return SDL_APP_CONTINUE;
			}
			start_drain_timer(p);
		} else if (ret != 0) {
			finish_current_file(p);
			return SDL_APP_CONTINUE;
		} else {
			schedule_next_frame(p);
		}

		print_status(p);
		return SDL_APP_CONTINUE;
	}

	if (event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
	    event->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
		/* Act on the fullscreen state change on next frame */
		p->nextmult = -1;
		ret = render_frame(p, 0);
		if (ret) {
			p->last_ret = ret;
			finish_current_file(p);
			return SDL_APP_CONTINUE;
		}
		if (event->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) {
			p->mouse_valid = 0;
			SDL_HideCursor();
		} else {
			SDL_ShowCursor();
		}
		print_status(p);
	} else if (event->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
		if (p->mouse_valid) {
			p->mouse_valid = 0;
			print_status(p);
		}
	} else if (event->type == SDL_EVENT_MOUSE_MOTION) {
		if (!p->fullscreen && p->ren && p->texw && p->texh) {
			float lx, ly;
			int nx = 0, ny = 0, valid = 0;

			if (SDL_RenderCoordinatesFromWindow(p->ren,
							    event->motion.x,
							    event->motion.y,
							    &lx, &ly)) {
				if (lx >= 0.0f && ly >= 0.0f) {
					nx = (int)lx;
					ny = (int)ly;
					if (nx < p->texw && ny < p->texh)
						valid = 1;
				}
			}

			if (valid != p->mouse_valid ||
			    (valid && (nx != p->mouse_x || ny != p->mouse_y))) {
				p->mouse_valid = valid;
				p->mouse_x = nx;
				p->mouse_y = ny;
				print_status(p);
			}
		}
	} else if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
		ret = render_frame(p, 0);
		if (ret) {
			p->last_ret = ret;
			finish_current_file(p);
		}
	} else if (event->type == SDL_EVENT_KEY_DOWN) {
		ke = (SDL_KeyboardEvent *)event;
		if (!ke->down || ke->repeat != 0)
			return SDL_APP_CONTINUE;

		if (ke->scancode == SDL_SCANCODE_SPACE && p->speedmode < 1) {
			if (p->paused && p->autopause) {
				p->paused = 0;
				p->autopause = 0;
			} else {
				p->paused ^= 1;
			}

			if (!p->paused) {
				p->next_disp_us += now_us() - p->ptick_us;
				if (p->as)
					SDL_ResumeAudioStreamDevice(p->as);
				schedule_next_frame(p);
			} else {
				stop_frame_timer(p);
				if (p->as)
					SDL_PauseAudioStreamDevice(p->as);
				p->ptick_us = now_us();
			}
		} else if (ke->scancode == SDL_SCANCODE_N) {
			if (p->as)
				SDL_ClearAudioStream(p->as);
			finish_current_file(p);
		} else if (ke->scancode == SDL_SCANCODE_PERIOD) {
			p->autopause = 1;
			if (p->paused) {
				p->paused = 0;
				p->next_disp_us += now_us() - p->ptick_us;
				if (p->as)
					SDL_ResumeAudioStreamDevice(p->as);
				schedule_next_frame(p);
			}
		} else if (ke->scancode == SDL_SCANCODE_Q) {
			if (p->as)
				SDL_ClearAudioStream(p->as);
			return SDL_APP_SUCCESS;
		} else if (ke->scancode == SDL_SCANCODE_F) {
			/*
			 * Queue fullscreen state change with SDL. Per SDL docs,
			 * this may be denied by the windowing system, otherwise
			 * a window state change event will be triggered.
			 */
			if (p->win)
				SDL_SetWindowFullscreen(p->win, !p->fullscreen);
		} else if (ke->scancode >= SDL_SCANCODE_1 &&
			   ke->scancode <= SDL_SCANCODE_6) {
			p->nextmult = ke->scancode - SDL_SCANCODE_1 + 1;
			ret = render_frame(p, 0);
			if (ret) {
				p->last_ret = ret;
				finish_current_file(p);
			}
		} else if (ke->scancode == SDL_SCANCODE_I) {
			p->sio.flags ^= SANDEC_FLAG_DO_FRAME_INTERPOLATION;
		} else if (ke->scancode == SDL_SCANCODE_L) {
			p->animv1full ^= 1;
		} else if (ke->scancode == SDL_SCANCODE_S) {
			p->texsmooth ^= 1;
			ret = render_frame(p, 0);
			if (ret) {
				p->last_ret = ret;
				finish_current_file(p);
			}
		} else if (ke->scancode == SDL_SCANCODE_G) {
			if (ke->mod & SDL_KMOD_SHIFT)
				p->grid_color = (p->grid_color + 1) % 5;
			else
				p->grid_on ^= 1;
			ret = render_frame(p, 0);
			if (ret) {
				p->last_ret = ret;
				finish_current_file(p);
			}
		} else if (ke->scancode == SDL_SCANCODE_H) {
			p->grid_corner ^= 1;
			if (p->grid_on) {
				ret = render_frame(p, 0);
				if (ret) {
					p->last_ret = ret;
					finish_current_file(p);
				}
			}
		}
		print_status(p);
	}

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
	struct playpriv *p = (struct playpriv *)appstate;
	int ret;

	if (!p->running && !p->draining_audio) {
		ret = open_next_file(p);
		if (ret < 0)
			return SDL_APP_FAILURE;
		if (ret == 0)
			return SDL_APP_SUCCESS;
	}

	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
	struct playpriv *p = (struct playpriv *)appstate;

	(void)result;

	if (!p)
		return;

	stop_frame_timer(p);
	stop_drain_timer(p);

	if (p->fhdl)
		FCLOSE(p->fhdl);
	if (p->sanctx)
		sandec_exit(&p->sanctx);
	if (p->surf)
		SDL_DestroySurface(p->surf);
	if (p->tex)
		SDL_DestroyTexture(p->tex);
	if (p->as)
		SDL_DestroyAudioStream(p->as);
	if (p->ren)
		SDL_DestroyRenderer(p->ren);
	if (p->win)
		SDL_DestroyWindow(p->win);
	if (p->verbose)
		putchar('\n');

	SDL_free(p);
}
