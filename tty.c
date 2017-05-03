/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "tmux.h"

static int tty_log_fd = -1;

void	tty_read_callback(struct bufferevent *, void *);
void	tty_error_callback(struct bufferevent *, short, void *);

static int tty_client_ready(struct client *, struct window_pane *);

void	tty_set_italics(struct tty *);
int	tty_try_colour(struct tty *, int, const char *);

void	tty_colours(struct tty *, const struct grid_cell *);
void	tty_check_fg(struct tty *, struct grid_cell *);
void	tty_check_bg(struct tty *, struct grid_cell *);
void	tty_colours_fg(struct tty *, const struct grid_cell *);
void	tty_colours_bg(struct tty *, const struct grid_cell *);

int	tty_large_region(struct tty *, const struct tty_ctx *);
int	tty_fake_bce(const struct tty *, const struct window_pane *);
void	tty_redraw_region(struct tty *, const struct tty_ctx *);
void	tty_emulate_repeat(struct tty *, enum tty_code_code, enum tty_code_code,
	    u_int);
void	tty_repeat_space(struct tty *, u_int);
void	tty_cell(struct tty *, const struct grid_cell *,
	    const struct window_pane *);
void	tty_default_colours(struct grid_cell *, const struct window_pane *);

#define tty_use_acs(tty) \
	(tty_term_has((tty)->term, TTYC_ACSC) && !((tty)->flags & TTY_UTF8))

#define tty_pane_full_width(tty, ctx) \
	((ctx)->xoff == 0 && screen_size_x((ctx)->wp->screen) >= (tty)->sx)

void
tty_create_log(void)
{
	char	name[64];

	xsnprintf(name, sizeof name, "tmux-out-%ld.log", (long)getpid());

	tty_log_fd = open(name, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (tty_log_fd != -1 && fcntl(tty_log_fd, F_SETFD, FD_CLOEXEC) == -1)
		fatal("fcntl failed");
}

int
tty_init(struct tty *tty, struct client *c, int fd, char *term)
{
	if (!isatty(fd))
		return (-1);

	memset(tty, 0, sizeof *tty);

	if (term == NULL || *term == '\0')
		tty->termname = xstrdup("unknown");
	else
		tty->termname = xstrdup(term);
	tty->fd = fd;
	tty->client = c;

	tty->cstyle = 0;
	tty->ccolour = xstrdup("");

	tty->flags = 0;
	tty->term_flags = 0;

	return (0);
}

int
tty_resize(struct tty *tty)
{
	struct client	*c = tty->client;
	struct winsize	 ws;
	u_int		 sx, sy;

	if (ioctl(tty->fd, TIOCGWINSZ, &ws) != -1) {
		sx = ws.ws_col;
		if (sx == 0)
			sx = 80;
		sy = ws.ws_row;
		if (sy == 0)
			sy = 24;
	} else {
		sx = 80;
		sy = 24;
	}
	if (!tty_set_size(tty, sx, sy))
		return (0);

	tty->cx = UINT_MAX;
	tty->cy = UINT_MAX;

	tty->rupper = UINT_MAX;
	tty->rlower = UINT_MAX;

	/*
	 * If the terminal has been started, reset the actual scroll region and
	 * cursor position, as this may not have happened.
	 */
	if (tty->flags & TTY_STARTED) {
		tty_cursor(tty, 0, 0);
		tty_region(tty, 0, tty->sy - 1);
	}

	return (1);
}

int
tty_set_size(struct tty *tty, u_int sx, u_int sy) {
	if (sx == tty->sx && sy == tty->sy)
		return (0);
	tty->sx = sx;
	tty->sy = sy;
	return (1);
}

int
tty_open(struct tty *tty, char **cause)
{
	tty->term = tty_term_find(tty->termname, tty->fd, cause);
	if (tty->term == NULL) {
		tty_close(tty);
		return (-1);
	}
	tty->flags |= TTY_OPENED;

	tty->flags &= ~(TTY_NOCURSOR|TTY_FREEZE|TTY_TIMER);

	tty->event = bufferevent_new(tty->fd, tty_read_callback, NULL,
	    tty_error_callback, tty);

	tty_start_tty(tty);

	tty_keys_build(tty);

	return (0);
}

void
tty_read_callback(__unused struct bufferevent *bufev, void *data)
{
	struct tty	*tty = data;
	struct client	*c = tty->client;

	while (tty_keys_next(tty))
		;
}

void
tty_error_callback(__unused struct bufferevent *bufev, __unused short what,
    __unused void *data)
{
}

void
tty_init_termios(int fd, struct termios *orig_tio, struct bufferevent *bufev)
{
	struct termios	tio;

	if (fd == -1 || tcgetattr(fd, orig_tio) != 0)
		return;

	setblocking(fd, 0);

	if (bufev != NULL)
		bufferevent_enable(bufev, EV_READ|EV_WRITE);

	memcpy(&tio, orig_tio, sizeof tio);
	tio.c_iflag &= ~(IXON|IXOFF|ICRNL|INLCR|IGNCR|IMAXBEL|ISTRIP);
	tio.c_iflag |= IGNBRK;
	tio.c_oflag &= ~(OPOST|ONLCR|OCRNL|ONLRET);
	tio.c_lflag &= ~(IEXTEN|ICANON|ECHO|ECHOE|ECHONL|ECHOCTL|
	    ECHOPRT|ECHOKE|ISIG);
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &tio) == 0)
		tcflush(fd, TCIOFLUSH);
}

void
tty_start_tty(struct tty *tty)
{
	tty_init_termios(tty->fd, &tty->tio, tty->event);

	tty_putcode(tty, TTYC_SMCUP);

	tty_putcode(tty, TTYC_SGR0);
	memcpy(&tty->cell, &grid_default_cell, sizeof tty->cell);

	tty_putcode(tty, TTYC_RMKX);
	if (tty_use_acs(tty))
		tty_putcode(tty, TTYC_ENACS);
	tty_putcode(tty, TTYC_CLEAR);

	tty_putcode(tty, TTYC_CNORM);
	if (tty_term_has(tty->term, TTYC_KMOUS))
		tty_puts(tty, "\033[?1000l\033[?1002l\033[?1006l\033[?1005l");

	if (tty_term_flag(tty->term, TTYC_XT)) {
		if (options_get_number(global_options, "focus-events")) {
			tty->flags |= TTY_FOCUS;
			tty_puts(tty, "\033[?1004h");
		}
	}

	tty->cx = UINT_MAX;
	tty->cy = UINT_MAX;

	tty->rlower = UINT_MAX;
	tty->rupper = UINT_MAX;

	tty->mode = MODE_CURSOR;

	tty->flags |= TTY_STARTED;

	tty_force_cursor_colour(tty, "");

	tty->mouse_drag_flag = 0;
	tty->mouse_drag_update = NULL;
	tty->mouse_drag_release = NULL;
}

void
tty_stop_tty(struct tty *tty)
{
	struct winsize	ws;

	if (!(tty->flags & TTY_STARTED))
		return;
	tty->flags &= ~TTY_STARTED;

	bufferevent_disable(tty->event, EV_READ|EV_WRITE);

	/*
	 * Be flexible about error handling and try not kill the server just
	 * because the fd is invalid. Things like ssh -t can easily leave us
	 * with a dead tty.
	 */
	if (ioctl(tty->fd, TIOCGWINSZ, &ws) == -1)
		return;
	if (tcsetattr(tty->fd, TCSANOW, &tty->tio) == -1)
		return;

	tty_raw(tty, tty_term_string2(tty->term, TTYC_CSR, 0, ws.ws_row - 1));
	if (tty_use_acs(tty))
		tty_raw(tty, tty_term_string(tty->term, TTYC_RMACS));
	tty_raw(tty, tty_term_string(tty->term, TTYC_SGR0));
	tty_raw(tty, tty_term_string(tty->term, TTYC_RMKX));
	tty_raw(tty, tty_term_string(tty->term, TTYC_CLEAR));
	if (tty_term_has(tty->term, TTYC_SS) && tty->cstyle != 0) {
		if (tty_term_has(tty->term, TTYC_SE))
			tty_raw(tty, tty_term_string(tty->term, TTYC_SE));
		else
			tty_raw(tty, tty_term_string1(tty->term, TTYC_SS, 0));
	}
	if (tty->mode & MODE_BRACKETPASTE)
		tty_raw(tty, "\033[?2004l");
	tty_raw(tty, tty_term_string(tty->term, TTYC_CR));

	tty_raw(tty, tty_term_string(tty->term, TTYC_CNORM));
	if (tty_term_has(tty->term, TTYC_KMOUS))
		tty_raw(tty, "\033[?1000l\033[?1002l\033[?1006l\033[?1005l");

	if (tty_term_flag(tty->term, TTYC_XT)) {
		if (tty->flags & TTY_FOCUS) {
			tty->flags &= ~TTY_FOCUS;
			tty_raw(tty, "\033[?1004l");
		}
	}

	tty_raw(tty, tty_term_string(tty->term, TTYC_RMCUP));

	setblocking(tty->fd, 1);
}

void
tty_close(struct tty *tty)
{
	if (event_initialized(&tty->key_timer))
		evtimer_del(&tty->key_timer);
	tty_stop_tty(tty);

	if (tty->flags & TTY_OPENED) {
		bufferevent_free(tty->event);

		tty_term_free(tty->term);
		tty_keys_free(tty);

		tty->flags &= ~TTY_OPENED;
	}

	if (tty->fd != -1) {
		close(tty->fd);
		tty->fd = -1;
	}
}

void
tty_free(struct tty *tty)
{
	tty_close(tty);

	free(tty->ccolour);
	free(tty->termname);
}

void
tty_raw(struct tty *tty, const char *s)
{
	ssize_t	n, slen;
	u_int	i;

	slen = strlen(s);
	for (i = 0; i < 5; i++) {
		n = write(tty->fd, s, slen);
		if (n >= 0) {
			s += n;
			slen -= n;
			if (slen == 0)
				break;
		} else if (n == -1 && errno != EAGAIN)
			break;
		usleep(100);
	}
}

void
tty_putcode(struct tty *tty, enum tty_code_code code)
{
	tty_puts(tty, tty_term_string(tty->term, code));
}

void
tty_putcode1(struct tty *tty, enum tty_code_code code, int a)
{
	if (a < 0)
		return;
	tty_puts(tty, tty_term_string1(tty->term, code, a));
}

void
tty_putcode2(struct tty *tty, enum tty_code_code code, int a, int b)
{
	if (a < 0 || b < 0)
		return;
	tty_puts(tty, tty_term_string2(tty->term, code, a, b));
}

void
tty_putcode_ptr1(struct tty *tty, enum tty_code_code code, const void *a)
{
	if (a != NULL)
		tty_puts(tty, tty_term_ptr1(tty->term, code, a));
}

void
tty_putcode_ptr2(struct tty *tty, enum tty_code_code code, const void *a,
    const void *b)
{
	if (a != NULL && b != NULL)
		tty_puts(tty, tty_term_ptr2(tty->term, code, a, b));
}

void
tty_puts(struct tty *tty, const char *s)
{
	if (*s == '\0')
		return;
	bufferevent_write(tty->event, s, strlen(s));

	if (tty_log_fd != -1)
		write(tty_log_fd, s, strlen(s));
}

void
tty_putc(struct tty *tty, u_char ch)
{
	const char	*acs;
	u_int		 sx;

	if (tty->cell.attr & GRID_ATTR_CHARSET) {
		acs = tty_acs_get(tty, ch);
		if (acs != NULL)
			bufferevent_write(tty->event, acs, strlen(acs));
		else
			bufferevent_write(tty->event, &ch, 1);
	} else
		bufferevent_write(tty->event, &ch, 1);

	if (ch >= 0x20 && ch != 0x7f) {
		sx = tty->sx;
		if (tty->term->flags & TERM_EARLYWRAP)
			sx--;

		if (tty->cx >= sx) {
			tty->cx = 1;
			if (tty->cy != tty->rlower)
				tty->cy++;
		} else
			tty->cx++;
	}

	if (tty_log_fd != -1)
		write(tty_log_fd, &ch, 1);
}

void
tty_putn(struct tty *tty, const void *buf, size_t len, u_int width)
{
	bufferevent_write(tty->event, buf, len);
	if (tty_log_fd != -1)
		write(tty_log_fd, buf, len);
	tty->cx += width;
}

void
tty_set_italics(struct tty *tty)
{
	const char	*s;

	if (tty_term_has(tty->term, TTYC_SITM)) {
		s = options_get_string(global_options, "default-terminal");
		if (strcmp(s, "screen") != 0 && strncmp(s, "screen-", 7) != 0) {
			tty_putcode(tty, TTYC_SITM);
			return;
		}
	}
	tty_putcode(tty, TTYC_SMSO);
}

void
tty_set_title(struct tty *tty, const char *title)
{
	if (!tty_term_has(tty->term, TTYC_TSL) ||
	    !tty_term_has(tty->term, TTYC_FSL))
		return;

	tty_putcode(tty, TTYC_TSL);
	tty_puts(tty, title);
	tty_putcode(tty, TTYC_FSL);
}

void
tty_force_cursor_colour(struct tty *tty, const char *ccolour)
{
	if (*ccolour == '\0')
		tty_putcode(tty, TTYC_CR);
	else
		tty_putcode_ptr1(tty, TTYC_CS, ccolour);
	free(tty->ccolour);
	tty->ccolour = xstrdup(ccolour);
}

void
tty_update_mode(struct tty *tty, int mode, struct screen *s)
{
	int	changed;

	if (s != NULL && strcmp(s->ccolour, tty->ccolour) != 0)
		tty_force_cursor_colour(tty, s->ccolour);

	if (tty->flags & TTY_NOCURSOR)
		mode &= ~MODE_CURSOR;

	changed = mode ^ tty->mode;
	if (changed & MODE_BLINKING) {
		if (tty_term_has(tty->term, TTYC_CVVIS))
			tty_putcode(tty, TTYC_CVVIS);
		else
			tty_putcode(tty, TTYC_CNORM);
		changed |= MODE_CURSOR;
	}
	if (changed & MODE_CURSOR) {
		if (mode & MODE_CURSOR)
			tty_putcode(tty, TTYC_CNORM);
		else
			tty_putcode(tty, TTYC_CIVIS);
	}
	if (s != NULL && tty->cstyle != s->cstyle) {
		if (tty_term_has(tty->term, TTYC_SS)) {
			if (s->cstyle == 0 &&
			    tty_term_has(tty->term, TTYC_SE))
				tty_putcode(tty, TTYC_SE);
			else
				tty_putcode1(tty, TTYC_SS, s->cstyle);
		}
		tty->cstyle = s->cstyle;
	}
	if (changed & ALL_MOUSE_MODES) {
		if (mode & ALL_MOUSE_MODES) {
			/*
			 * Enable the SGR (1006) extension unconditionally, as
			 * this is safe from misinterpretation. Do it in this
			 * order, because in some terminals it's the last one
			 * that takes effect and SGR is the preferred one.
			 */
			tty_puts(tty, "\033[?1006h");
			if (mode & MODE_MOUSE_BUTTON)
				tty_puts(tty, "\033[?1002h");
			else if (mode & MODE_MOUSE_STANDARD)
				tty_puts(tty, "\033[?1000h");
		} else {
			if (tty->mode & MODE_MOUSE_BUTTON)
				tty_puts(tty, "\033[?1002l");
			else if (tty->mode & MODE_MOUSE_STANDARD)
				tty_puts(tty, "\033[?1000l");
			tty_puts(tty, "\033[?1006l");
		}
	}
	if (changed & MODE_KKEYPAD) {
		if (mode & MODE_KKEYPAD)
			tty_putcode(tty, TTYC_SMKX);
		else
			tty_putcode(tty, TTYC_RMKX);
	}
	if (changed & MODE_BRACKETPASTE) {
		if (mode & MODE_BRACKETPASTE)
			tty_puts(tty, "\033[?2004h");
		else
			tty_puts(tty, "\033[?2004l");
	}
	tty->mode = mode;
}

void
tty_emulate_repeat(struct tty *tty, enum tty_code_code code,
    enum tty_code_code code1, u_int n)
{
	if (tty_term_has(tty->term, code))
		tty_putcode1(tty, code, n);
	else {
		while (n-- > 0)
			tty_putcode(tty, code1);
	}
}

void
tty_repeat_space(struct tty *tty, u_int n)
{
	while (n-- > 0)
		tty_putc(tty, ' ');
}

/*
 * Is the region large enough to be worth redrawing once later rather than
 * probably several times now? Currently yes if it is more than 50% of the
 * pane.
 */
int
tty_large_region(__unused struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;

	return (ctx->orlower - ctx->orupper >= screen_size_y(wp->screen) / 2);
}

/*
 * Return if BCE is needed but the terminal doesn't have it - it'll need to be
 * emulated.
 */
int
tty_fake_bce(const struct tty *tty, const struct window_pane *wp)
{
	struct grid_cell	gc;

	memcpy(&gc, &grid_default_cell, sizeof gc);
	if (wp != NULL)
		tty_default_colours(&gc, wp);

	if (gc.bg == 8)
		return (0);
	return (!tty_term_flag(tty->term, TTYC_BCE));
}

/*
 * Redraw scroll region using data from screen (already updated). Used when
 * CSR not supported, or window is a pane that doesn't take up the full
 * width of the terminal.
 */
void
tty_redraw_region(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;
	u_int			 i;

	/*
	 * If region is large, schedule a window redraw. In most cases this is
	 * likely to be followed by some more scrolling.
	 */
	if (tty_large_region(tty, ctx)) {
		wp->flags |= PANE_REDRAW;
		return;
	}

	if (ctx->ocy < ctx->orupper || ctx->ocy > ctx->orlower) {
		for (i = ctx->ocy; i < screen_size_y(s); i++)
			tty_draw_pane(tty, wp, i, ctx->xoff, ctx->yoff);
	} else {
		for (i = ctx->orupper; i <= ctx->orlower; i++)
			tty_draw_pane(tty, wp, i, ctx->xoff, ctx->yoff);
	}
}

void
tty_draw_pane(struct tty *tty, const struct window_pane *wp, u_int py, u_int ox,
    u_int oy)
{
	tty_draw_line(tty, wp, wp->screen, py, ox, oy);
}

void
tty_draw_line(struct tty *tty, const struct window_pane *wp,
    struct screen *s, u_int py, u_int ox, u_int oy)
{
	struct grid_cell	 gc, tmp_gc;
	struct grid_line	*gl;
	u_int			 i, sx;
	int			 flags;

	flags = tty->flags & TTY_NOCURSOR;
	tty->flags |= TTY_NOCURSOR;
	tty_update_mode(tty, tty->mode, s);

	sx = screen_size_x(s);
	if (sx > s->grid->linedata[s->grid->hsize + py].cellsize)
		sx = s->grid->linedata[s->grid->hsize + py].cellsize;
	if (sx > tty->sx)
		sx = tty->sx;

	/*
	 * Don't move the cursor to the start position if it will wrap there
	 * itself.
	 */
	gl = NULL;
	if (py != 0)
		gl = &s->grid->linedata[s->grid->hsize + py - 1];
	if (oy + py == 0 || gl == NULL || !(gl->flags & GRID_LINE_WRAPPED) ||
	    tty->cx < tty->sx || ox != 0 ||
	    (oy + py != tty->cy + 1 && tty->cy != s->rlower + oy))
		tty_cursor(tty, ox, oy + py);

	for (i = 0; i < sx; i++) {
		grid_view_get_cell(s->grid, i, py, &gc);
		if (gc.flags & GRID_FLAG_SELECTED) {
			screen_select_cell(s, &tmp_gc, &gc);
			tty_cell(tty, &tmp_gc, wp);
		} else
			tty_cell(tty, &gc, wp);
	}

	if (sx < tty->sx) {
		tty_attributes(tty, &grid_default_cell, wp);

		tty_cursor(tty, ox + sx, oy + py);
		if (sx != screen_size_x(s) &&
		    ox + screen_size_x(s) >= tty->sx &&
		    tty_term_has(tty->term, TTYC_EL) &&
		    !tty_fake_bce(tty, wp))
			tty_putcode(tty, TTYC_EL);
		else
			tty_repeat_space(tty, screen_size_x(s) - sx);
	}

	tty->flags = (tty->flags & ~TTY_NOCURSOR) | flags;
	tty_update_mode(tty, tty->mode, s);
}

static int
tty_client_ready(struct client *c, struct window_pane *wp)
{
	if (c->session == NULL || c->tty.term == NULL)
		return (0);
	if (c->flags & CLIENT_SUSPENDED)
		return (0);
	if (c->tty.flags & TTY_FREEZE)
		return (0);
	if (c->session->curw->window != wp->window)
		return (0);
	return (1);
}

void
tty_write(void (*cmdfn)(struct tty *, const struct tty_ctx *),
    struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct client		*c;

	/* wp can be NULL if updating the screen but not the terminal. */
	if (wp == NULL)
		return;

	if (wp->window->flags & WINDOW_REDRAW || wp->flags & PANE_REDRAW)
		return;
	if (!window_pane_visible(wp) || wp->flags & PANE_DROP)
		return;

	TAILQ_FOREACH(c, &clients, entry) {
		if (!tty_client_ready(c, wp))
			continue;

		ctx->xoff = wp->xoff;
		ctx->yoff = wp->yoff;
		if (status_at_line(c) == 0)
			ctx->yoff++;

		cmdfn(&c->tty, ctx);
	}
}

void
tty_cmd_insertcharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;

	if (!tty_pane_full_width(tty, ctx)) {
		tty_draw_pane(tty, wp, ctx->ocy, ctx->xoff, ctx->yoff);
		return;
	}

	tty_attributes(tty, &grid_default_cell, wp);

	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	if (!tty_fake_bce(tty, wp) && (tty_term_has(tty->term, TTYC_ICH) ||
	    tty_term_has(tty->term, TTYC_ICH1)))
		tty_emulate_repeat(tty, TTYC_ICH, TTYC_ICH1, ctx->num);
	else
		tty_draw_pane(tty, wp, ctx->ocy, ctx->xoff, ctx->yoff);
}

void
tty_cmd_deletecharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;

	if (!tty_pane_full_width(tty, ctx) || tty_fake_bce(tty, wp) ||
	    (!tty_term_has(tty->term, TTYC_DCH) &&
	    !tty_term_has(tty->term, TTYC_DCH1))) {
		tty_draw_pane(tty, wp, ctx->ocy, ctx->xoff, ctx->yoff);
		return;
	}

	tty_attributes(tty, &grid_default_cell, wp);

	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	if (tty_term_has(tty->term, TTYC_DCH) ||
	    tty_term_has(tty->term, TTYC_DCH1))
		tty_emulate_repeat(tty, TTYC_DCH, TTYC_DCH1, ctx->num);
}

void
tty_cmd_clearcharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	u_int	i;

	tty_attributes(tty, &grid_default_cell, ctx->wp);

	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	if (tty_term_has(tty->term, TTYC_ECH) && !tty_fake_bce(tty, ctx->wp))
		tty_putcode1(tty, TTYC_ECH, ctx->num);
	else {
		for (i = 0; i < ctx->num; i++)
			tty_putc(tty, ' ');
	}
}

void
tty_cmd_insertline(struct tty *tty, const struct tty_ctx *ctx)
{
	if (!tty_pane_full_width(tty, ctx) || tty_fake_bce(tty, ctx->wp) ||
	    !tty_term_has(tty->term, TTYC_CSR) ||
	    !tty_term_has(tty->term, TTYC_IL1)) {
		tty_redraw_region(tty, ctx);
		return;
	}

	tty_attributes(tty, &grid_default_cell, ctx->wp);

	tty_region_pane(tty, ctx, ctx->orupper, ctx->orlower);
	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	tty_emulate_repeat(tty, TTYC_IL, TTYC_IL1, ctx->num);
}

void
tty_cmd_deleteline(struct tty *tty, const struct tty_ctx *ctx)
{
	if (!tty_pane_full_width(tty, ctx) || tty_fake_bce(tty, ctx->wp) ||
	    !tty_term_has(tty->term, TTYC_CSR) ||
	    !tty_term_has(tty->term, TTYC_DL1)) {
		tty_redraw_region(tty, ctx);
		return;
	}

	tty_attributes(tty, &grid_default_cell, ctx->wp);

	tty_region_pane(tty, ctx, ctx->orupper, ctx->orlower);
	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	tty_emulate_repeat(tty, TTYC_DL, TTYC_DL1, ctx->num);
}

void
tty_cmd_clearline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_cursor_pane(tty, ctx, 0, ctx->ocy);

	if (tty_pane_full_width(tty, ctx) && !tty_fake_bce(tty, wp) &&
	    tty_term_has(tty->term, TTYC_EL))
		tty_putcode(tty, TTYC_EL);
	else
		tty_repeat_space(tty, screen_size_x(s));
}

void
tty_cmd_clearendofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	if (tty_pane_full_width(tty, ctx) &&
	    tty_term_has(tty->term, TTYC_EL) && !tty_fake_bce(tty, wp))
		tty_putcode(tty, TTYC_EL);
	else
		tty_repeat_space(tty, screen_size_x(s) - ctx->ocx);
}

void
tty_cmd_clearstartofline(struct tty *tty, const struct tty_ctx *ctx)
{
	tty_attributes(tty, &grid_default_cell, ctx->wp);

	if (ctx->xoff == 0 && tty_term_has(tty->term, TTYC_EL1) &&
	    !tty_fake_bce(tty, ctx->wp)) {
		tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);
		tty_putcode(tty, TTYC_EL1);
	} else {
		tty_cursor_pane(tty, ctx, 0, ctx->ocy);
		tty_repeat_space(tty, ctx->ocx + 1);
	}
}

void
tty_cmd_reverseindex(struct tty *tty, const struct tty_ctx *ctx)
{
	if (ctx->ocy != ctx->orupper)
		return;

	if (!tty_pane_full_width(tty, ctx) || tty_fake_bce(tty, ctx->wp) ||
	    !tty_term_has(tty->term, TTYC_CSR) ||
	    !tty_term_has(tty->term, TTYC_RI)) {
		tty_redraw_region(tty, ctx);
		return;
	}

	tty_attributes(tty, &grid_default_cell, ctx->wp);

	tty_region_pane(tty, ctx, ctx->orupper, ctx->orlower);
	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->orupper);

	tty_putcode(tty, TTYC_RI);
}

void
tty_cmd_linefeed(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;

	if (ctx->ocy != ctx->orlower)
		return;

	if (!tty_pane_full_width(tty, ctx) || tty_fake_bce(tty, wp) ||
	    !tty_term_has(tty->term, TTYC_CSR)) {
		if (tty_large_region(tty, ctx))
			wp->flags |= PANE_REDRAW;
		else
			tty_redraw_region(tty, ctx);
		return;
	}

	/*
	 * If this line wrapped naturally (ctx->num is nonzero), don't do
	 * anything - the cursor can just be moved to the last cell and wrap
	 * naturally.
	 */
	if (ctx->num && !(tty->term->flags & TERM_EARLYWRAP))
		return;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_region_pane(tty, ctx, ctx->orupper, ctx->orlower);
	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	tty_putc(tty, '\n');
}

void
tty_cmd_clearendofscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;
	u_int			 i, j;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_region_pane(tty, ctx, 0, screen_size_y(s) - 1);
	tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	if (tty_pane_full_width(tty, ctx) &&
	    tty_term_has(tty->term, TTYC_EL) && !tty_fake_bce(tty, wp)) {
		tty_putcode(tty, TTYC_EL);
		if (ctx->ocy != screen_size_y(s) - 1) {
			tty_cursor_pane(tty, ctx, 0, ctx->ocy + 1);
			for (i = ctx->ocy + 1; i < screen_size_y(s); i++) {
				tty_putcode(tty, TTYC_EL);
				if (i == screen_size_y(s) - 1)
					continue;
				tty_emulate_repeat(tty, TTYC_CUD, TTYC_CUD1, 1);
				tty->cy++;
			}
		}
	} else {
		tty_repeat_space(tty, screen_size_x(s) - ctx->ocx);
		for (j = ctx->ocy + 1; j < screen_size_y(s); j++) {
			tty_cursor_pane(tty, ctx, 0, j);
			tty_repeat_space(tty, screen_size_x(s));
		}
	}
}

void
tty_cmd_clearstartofscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;
	u_int			 i, j;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_region_pane(tty, ctx, 0, screen_size_y(s) - 1);
	tty_cursor_pane(tty, ctx, 0, 0);

	if (tty_pane_full_width(tty, ctx) &&
	    tty_term_has(tty->term, TTYC_EL) && !tty_fake_bce(tty, wp)) {
		for (i = 0; i < ctx->ocy; i++) {
			tty_putcode(tty, TTYC_EL);
			tty_emulate_repeat(tty, TTYC_CUD, TTYC_CUD1, 1);
			tty->cy++;
		}
	} else {
		for (j = 0; j < ctx->ocy; j++) {
			tty_cursor_pane(tty, ctx, 0, j);
			tty_repeat_space(tty, screen_size_x(s));
		}
	}
	tty_repeat_space(tty, ctx->ocx + 1);
}

void
tty_cmd_clearscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;
	u_int			 i, j;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_region_pane(tty, ctx, 0, screen_size_y(s) - 1);
	tty_cursor_pane(tty, ctx, 0, 0);

	if (tty_pane_full_width(tty, ctx) &&
	    tty_term_has(tty->term, TTYC_EL) && !tty_fake_bce(tty, wp)) {
		for (i = 0; i < screen_size_y(s); i++) {
			tty_putcode(tty, TTYC_EL);
			if (i != screen_size_y(s) - 1) {
				tty_emulate_repeat(tty, TTYC_CUD, TTYC_CUD1, 1);
				tty->cy++;
			}
		}
	} else {
		for (j = 0; j < screen_size_y(s); j++) {
			tty_cursor_pane(tty, ctx, 0, j);
			tty_repeat_space(tty, screen_size_x(s));
		}
	}
}

void
tty_cmd_alignmenttest(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;
	u_int			 i, j;

	tty_attributes(tty, &grid_default_cell, wp);

	tty_region_pane(tty, ctx, 0, screen_size_y(s) - 1);

	for (j = 0; j < screen_size_y(s); j++) {
		tty_cursor_pane(tty, ctx, 0, j);
		for (i = 0; i < screen_size_x(s); i++)
			tty_putc(tty, 'E');
	}
}

void
tty_cmd_cell(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;
	struct screen		*s = wp->screen;
	u_int			 cx, width;

	if (ctx->ocy == ctx->orlower)
		tty_region_pane(tty, ctx, ctx->orupper, ctx->orlower);

	/* Is the cursor in the very last position? */
	width = ctx->cell->data.width;
	if (ctx->ocx > wp->sx - width) {
		if (ctx->xoff != 0 || wp->sx != tty->sx) {
			/*
			 * The pane doesn't fill the entire line, the linefeed
			 * will already have happened, so just move the cursor.
			 */
			if (ctx->ocy != wp->yoff + wp->screen->rlower)
				tty_cursor_pane(tty, ctx, 0, ctx->ocy + 1);
			else
				tty_cursor_pane(tty, ctx, 0, ctx->ocy);
		} else if (tty->cx < tty->sx) {
			/*
			 * The cursor isn't in the last position already, so
			 * move as far left as possible and redraw the last
			 * cell to move into the last position.
			 */
			cx = screen_size_x(s) - ctx->last_cell.data.width;
			tty_cursor_pane(tty, ctx, cx, ctx->ocy);
			tty_cell(tty, &ctx->last_cell, wp);
		}
	} else
		tty_cursor_pane(tty, ctx, ctx->ocx, ctx->ocy);

	tty_cell(tty, ctx->cell, wp);
}

void
tty_cmd_utf8character(struct tty *tty, const struct tty_ctx *ctx)
{
	struct window_pane	*wp = ctx->wp;

	/*
	 * Cannot rely on not being a partial character, so just redraw the
	 * whole line.
	 */
	tty_draw_pane(tty, wp, ctx->ocy, ctx->xoff, ctx->yoff);
}

void
tty_cmd_setselection(struct tty *tty, const struct tty_ctx *ctx)
{
	char	*buf;
	size_t	 off;

	if (!tty_term_has(tty->term, TTYC_MS))
		return;

	off = 4 * ((ctx->num + 2) / 3) + 1; /* storage for base64 */
	buf = xmalloc(off);

	b64_ntop(ctx->ptr, ctx->num, buf, off);
	tty_putcode_ptr2(tty, TTYC_MS, "", buf);

	free(buf);
}

void
tty_cmd_rawstring(struct tty *tty, const struct tty_ctx *ctx)
{
	u_int	 i;
	u_char	*str = ctx->ptr;

	for (i = 0; i < ctx->num; i++)
		tty_putc(tty, str[i]);

	tty->cx = tty->cy = UINT_MAX;
	tty->rupper = tty->rlower = UINT_MAX;

	tty_attributes(tty, &grid_default_cell, ctx->wp);
	tty_cursor(tty, 0, 0);
}

void
tty_cell(struct tty *tty, const struct grid_cell *gc,
    const struct window_pane *wp)
{
	u_int	i;

	/* Skip last character if terminal is stupid. */
	if (tty->term->flags & TERM_EARLYWRAP &&
	    tty->cy == tty->sy - 1 && tty->cx == tty->sx - 1)
		return;

	/* If this is a padding character, do nothing. */
	if (gc->flags & GRID_FLAG_PADDING)
		return;

	/* Set the attributes. */
	tty_attributes(tty, gc, wp);

	/* Get the cell and if ASCII write with putc to do ACS translation. */
	if (gc->data.size == 1) {
		if (*gc->data.data < 0x20 || *gc->data.data == 0x7f)
			return;
		tty_putc(tty, *gc->data.data);
		return;
	}

	/* If not UTF-8, write _. */
	if (!(tty->flags & TTY_UTF8)) {
		for (i = 0; i < gc->data.width; i++)
			tty_putc(tty, '_');
		return;
	}

	/* Write the data. */
	tty_putn(tty, gc->data.data, gc->data.size, gc->data.width);
}

void
tty_reset(struct tty *tty)
{
	struct grid_cell	*gc = &tty->cell;

	if (grid_cells_equal(gc, &grid_default_cell))
		return;

	if ((gc->attr & GRID_ATTR_CHARSET) && tty_use_acs(tty))
		tty_putcode(tty, TTYC_RMACS);
	tty_putcode(tty, TTYC_SGR0);
	memcpy(gc, &grid_default_cell, sizeof *gc);
}

/* Set region inside pane. */
void
tty_region_pane(struct tty *tty, const struct tty_ctx *ctx, u_int rupper,
    u_int rlower)
{
	tty_region(tty, ctx->yoff + rupper, ctx->yoff + rlower);
}

/* Set region at absolute position. */
void
tty_region(struct tty *tty, u_int rupper, u_int rlower)
{
	if (tty->rlower == rlower && tty->rupper == rupper)
		return;
	if (!tty_term_has(tty->term, TTYC_CSR))
		return;

	tty->rupper = rupper;
	tty->rlower = rlower;

	/*
	 * Some terminals (such as PuTTY) do not correctly reset the cursor to
	 * 0,0 if it is beyond the last column (they do not reset their wrap
	 * flag so further output causes a line feed). As a workaround, do an
	 * explicit move to 0 first.
	 */
	if (tty->cx >= tty->sx)
		tty_cursor(tty, 0, tty->cy);

	tty_putcode2(tty, TTYC_CSR, tty->rupper, tty->rlower);
	tty_cursor(tty, 0, 0);
}

/* Move cursor inside pane. */
void
tty_cursor_pane(struct tty *tty, const struct tty_ctx *ctx, u_int cx, u_int cy)
{
	tty_cursor(tty, ctx->xoff + cx, ctx->yoff + cy);
}

/* Move cursor to absolute position. */
void
tty_cursor(struct tty *tty, u_int cx, u_int cy)
{
	struct tty_term	*term = tty->term;
	u_int		 thisx, thisy;
	int		 change;

	if (cx > tty->sx - 1)
		cx = tty->sx - 1;

	thisx = tty->cx;
	thisy = tty->cy;

	/* No change. */
	if (cx == thisx && cy == thisy)
		return;

	/* Very end of the line, just use absolute movement. */
	if (thisx > tty->sx - 1)
		goto absolute;

	/* Move to home position (0, 0). */
	if (cx == 0 && cy == 0 && tty_term_has(term, TTYC_HOME)) {
		tty_putcode(tty, TTYC_HOME);
		goto out;
	}

	/* Zero on the next line. */
	if (cx == 0 && cy == thisy + 1 && thisy != tty->rlower) {
		tty_putc(tty, '\r');
		tty_putc(tty, '\n');
		goto out;
	}

	/* Moving column or row. */
	if (cy == thisy) {
		/*
		 * Moving column only, row staying the same.
		 */

		/* To left edge. */
		if (cx == 0) {
			tty_putc(tty, '\r');
			goto out;
		}

		/* One to the left. */
		if (cx == thisx - 1 && tty_term_has(term, TTYC_CUB1)) {
			tty_putcode(tty, TTYC_CUB1);
			goto out;
		}

		/* One to the right. */
		if (cx == thisx + 1 && tty_term_has(term, TTYC_CUF1)) {
			tty_putcode(tty, TTYC_CUF1);
			goto out;
		}

		/* Calculate difference. */
		change = thisx - cx;	/* +ve left, -ve right */

		/*
		 * Use HPA if change is larger than absolute, otherwise move
		 * the cursor with CUB/CUF.
		 */
		if ((u_int) abs(change) > cx && tty_term_has(term, TTYC_HPA)) {
			tty_putcode1(tty, TTYC_HPA, cx);
			goto out;
		} else if (change > 0 && tty_term_has(term, TTYC_CUB)) {
			if (change == 2 && tty_term_has(term, TTYC_CUB1)) {
				tty_putcode(tty, TTYC_CUB1);
				tty_putcode(tty, TTYC_CUB1);
				goto out;
			}
			tty_putcode1(tty, TTYC_CUB, change);
			goto out;
		} else if (change < 0 && tty_term_has(term, TTYC_CUF)) {
			tty_putcode1(tty, TTYC_CUF, -change);
			goto out;
		}
	} else if (cx == thisx) {
		/*
		 * Moving row only, column staying the same.
		 */

		/* One above. */
		if (thisy != tty->rupper &&
		    cy == thisy - 1 && tty_term_has(term, TTYC_CUU1)) {
			tty_putcode(tty, TTYC_CUU1);
			goto out;
		}

		/* One below. */
		if (thisy != tty->rlower &&
		    cy == thisy + 1 && tty_term_has(term, TTYC_CUD1)) {
			tty_putcode(tty, TTYC_CUD1);
			goto out;
		}

		/* Calculate difference. */
		change = thisy - cy;	/* +ve up, -ve down */

		/*
		 * Try to use VPA if change is larger than absolute or if this
		 * change would cross the scroll region, otherwise use CUU/CUD.
		 */
		if ((u_int) abs(change) > cy ||
		    (change < 0 && cy - change > tty->rlower) ||
		    (change > 0 && cy - change < tty->rupper)) {
			    if (tty_term_has(term, TTYC_VPA)) {
				    tty_putcode1(tty, TTYC_VPA, cy);
				    goto out;
			    }
		} else if (change > 0 && tty_term_has(term, TTYC_CUU)) {
			tty_putcode1(tty, TTYC_CUU, change);
			goto out;
		} else if (change < 0 && tty_term_has(term, TTYC_CUD)) {
			tty_putcode1(tty, TTYC_CUD, -change);
			goto out;
		}
	}

absolute:
	/* Absolute movement. */
	tty_putcode2(tty, TTYC_CUP, cy, cx);

out:
	tty->cx = cx;
	tty->cy = cy;
}

void
tty_attributes(struct tty *tty, const struct grid_cell *gc,
    const struct window_pane *wp)
{
	struct grid_cell	*tc = &tty->cell, gc2;
	u_char			 changed;

	memcpy(&gc2, gc, sizeof gc2);
	if (wp != NULL)
		tty_default_colours(&gc2, wp);

	/*
	 * If no setab, try to use the reverse attribute as a best-effort for a
	 * non-default background. This is a bit of a hack but it doesn't do
	 * any serious harm and makes a couple of applications happier.
	 */
	if (!tty_term_has(tty->term, TTYC_SETAB)) {
		if (gc2.attr & GRID_ATTR_REVERSE) {
			if (gc2.fg != 7 && gc2.fg != 8)
				gc2.attr &= ~GRID_ATTR_REVERSE;
		} else {
			if (gc2.bg != 0 && gc2.bg != 8)
				gc2.attr |= GRID_ATTR_REVERSE;
		}
	}

	/* Fix up the colours if necessary. */
	tty_check_fg(tty, &gc2);
	tty_check_bg(tty, &gc2);

	/* If any bits are being cleared, reset everything. */
	if (tc->attr & ~gc2.attr)
		tty_reset(tty);

	/*
	 * Set the colours. This may call tty_reset() (so it comes next) and
	 * may add to (NOT remove) the desired attributes by changing new_attr.
	 */
	tty_colours(tty, &gc2);

	/* Filter out attribute bits already set. */
	changed = gc2.attr & ~tc->attr;
	tc->attr = gc2.attr;

	/* Set the attributes. */
	if (changed & GRID_ATTR_BRIGHT)
		tty_putcode(tty, TTYC_BOLD);
	if (changed & GRID_ATTR_DIM)
		tty_putcode(tty, TTYC_DIM);
	if (changed & GRID_ATTR_ITALICS)
		tty_set_italics(tty);
	if (changed & GRID_ATTR_UNDERSCORE)
		tty_putcode(tty, TTYC_SMUL);
	if (changed & GRID_ATTR_BLINK)
		tty_putcode(tty, TTYC_BLINK);
	if (changed & GRID_ATTR_REVERSE) {
		if (tty_term_has(tty->term, TTYC_REV))
			tty_putcode(tty, TTYC_REV);
		else if (tty_term_has(tty->term, TTYC_SMSO))
			tty_putcode(tty, TTYC_SMSO);
	}
	if (changed & GRID_ATTR_HIDDEN)
		tty_putcode(tty, TTYC_INVIS);
	if ((changed & GRID_ATTR_CHARSET) && tty_use_acs(tty))
		tty_putcode(tty, TTYC_SMACS);
}

void
tty_colours(struct tty *tty, const struct grid_cell *gc)
{
	struct grid_cell	*tc = &tty->cell;
	int			 have_ax;

	/* No changes? Nothing is necessary. */
	if (gc->fg == tc->fg && gc->bg == tc->bg)
		return;

	/*
	 * Is either the default colour? This is handled specially because the
	 * best solution might be to reset both colours to default, in which
	 * case if only one is default need to fall onward to set the other
	 * colour.
	 */
	if (gc->fg == 8 || gc->bg == 8) {
		/*
		 * If don't have AX but do have op, send sgr0 (op can't
		 * actually be used because it is sometimes the same as sgr0
		 * and sometimes isn't). This resets both colours to default.
		 *
		 * Otherwise, try to set the default colour only as needed.
		 */
		have_ax = tty_term_flag(tty->term, TTYC_AX);
		if (!have_ax && tty_term_has(tty->term, TTYC_OP))
			tty_reset(tty);
		else {
			if (gc->fg == 8 && tc->fg != 8) {
				if (have_ax)
					tty_puts(tty, "\033[39m");
				else if (tc->fg != 7)
					tty_putcode1(tty, TTYC_SETAF, 7);
				tc->fg = 8;
			}
			if (gc->bg == 8 && tc->bg != 8) {
				if (have_ax)
					tty_puts(tty, "\033[49m");
				else if (tc->bg != 0)
					tty_putcode1(tty, TTYC_SETAB, 0);
				tc->bg = 8;
			}
		}
	}

	/* Set the foreground colour. */
	if (gc->fg != 8 && gc->fg != tc->fg)
		tty_colours_fg(tty, gc);

	/*
	 * Set the background colour. This must come after the foreground as
	 * tty_colour_fg() can call tty_reset().
	 */
	if (gc->bg != 8 && gc->bg != tc->bg)
		tty_colours_bg(tty, gc);
}

void
tty_check_fg(struct tty *tty, struct grid_cell *gc)
{
	u_char	r, g, b;
	u_int	colours;

	/* Is this a 24-bit colour? */
	if (gc->fg & COLOUR_FLAG_RGB) {
		/* Not a 24-bit terminal? Translate to 256-colour palette. */
		if (!tty_term_flag(tty->term, TTYC_TC)) {
			colour_split_rgb(gc->fg, &r, &g, &b);
			gc->fg = colour_find_rgb(r, g, b);
		} else
			return;
	}
	colours = tty_term_number(tty->term, TTYC_COLORS);

	/* Is this a 256-colour colour? */
	if (gc->fg & COLOUR_FLAG_256) {
		/* And not a 256 colour mode? */
		if (!(tty->term->flags & TERM_256COLOURS) &&
		    !(tty->term_flags & TERM_256COLOURS)) {
			gc->fg = colour_256to16(gc->fg);
			if (gc->fg & 8) {
				gc->fg &= 7;
				if (colours >= 16)
					gc->fg += 90;
				else
					gc->attr |= GRID_ATTR_BRIGHT;
			} else
				gc->attr &= ~GRID_ATTR_BRIGHT;
		}
		return;
	}

	/* Is this an aixterm colour? */
	if (gc->fg >= 90 && gc->fg <= 97 && colours < 16) {
		gc->fg -= 90;
		gc->attr |= GRID_ATTR_BRIGHT;
	}
}

void
tty_check_bg(struct tty *tty, struct grid_cell *gc)
{
	u_char	r, g, b;
	u_int	colours;

	/* Is this a 24-bit colour? */
	if (gc->bg & COLOUR_FLAG_RGB) {
		/* Not a 24-bit terminal? Translate to 256-colour palette. */
		if (!tty_term_flag(tty->term, TTYC_TC)) {
			colour_split_rgb(gc->bg, &r, &g, &b);
			gc->bg = colour_find_rgb(r, g, b);
		} else
			return;
	}
	colours = tty_term_number(tty->term, TTYC_COLORS);

	/* Is this a 256-colour colour? */
	if (gc->bg & COLOUR_FLAG_256) {
		/*
		 * And not a 256 colour mode? Translate to 16-colour
		 * palette. Bold background doesn't exist portably, so just
		 * discard the bold bit if set.
		 */
		if (!(tty->term->flags & TERM_256COLOURS) &&
		    !(tty->term_flags & TERM_256COLOURS)) {
			gc->bg = colour_256to16(gc->bg);
			if (gc->bg & 8) {
				gc->bg &= 7;
				if (colours >= 16)
					gc->fg += 90;
			}
		}
		return;
	}

	/* Is this an aixterm colour? */
	if (gc->bg >= 90 && gc->bg <= 97 && colours < 16)
		gc->bg -= 90;
}

void
tty_colours_fg(struct tty *tty, const struct grid_cell *gc)
{
	struct grid_cell	*tc = &tty->cell;
	char			 s[32];

	/* Is this a 24-bit or 256-colour colour? */
	if (gc->fg & COLOUR_FLAG_RGB ||
	    gc->fg & COLOUR_FLAG_256) {
		if (tty_try_colour(tty, gc->fg, "38") == 0)
			goto save_fg;
		/* Should not get here, already converted in tty_check_fg. */
		return;
	}

	/* Is this an aixterm bright colour? */
	if (gc->fg >= 90 && gc->fg <= 97) {
		xsnprintf(s, sizeof s, "\033[%dm", gc->fg);
		tty_puts(tty, s);
		goto save_fg;
	}

	/* Otherwise set the foreground colour. */
	tty_putcode1(tty, TTYC_SETAF, gc->fg);

save_fg:
	/* Save the new values in the terminal current cell. */
	tc->fg = gc->fg;
}

void
tty_colours_bg(struct tty *tty, const struct grid_cell *gc)
{
	struct grid_cell	*tc = &tty->cell;
	char			 s[32];

	/* Is this a 24-bit or 256-colour colour? */
	if (gc->bg & COLOUR_FLAG_RGB ||
	    gc->bg & COLOUR_FLAG_256) {
		if (tty_try_colour(tty, gc->bg, "48") == 0)
			goto save_bg;
		/* Should not get here, already converted in tty_check_bg. */
		return;
	}

	/* Is this an aixterm bright colour? */
	if (gc->bg >= 90 && gc->bg <= 97) {
		xsnprintf(s, sizeof s, "\033[%dm", gc->bg + 10);
		tty_puts(tty, s);
		goto save_bg;
	}

	/* Otherwise set the background colour. */
	tty_putcode1(tty, TTYC_SETAB, gc->bg);

save_bg:
	/* Save the new values in the terminal current cell. */
	tc->bg = gc->bg;
}

int
tty_try_colour(struct tty *tty, int colour, const char *type)
{
	u_char	r, g, b;
	char	s[32];

	if (colour & COLOUR_FLAG_256) {
		/*
		 * If the user has specified -2 to the client, setaf and setab
		 * may not work (or they may not want to use them), so send the
		 * usual sequence.
		 */
		if (tty->term_flags & TERM_256COLOURS)
			goto fallback_256;

		/*
		 * If the terminfo entry has 256 colours and setaf and setab
		 * exist, assume that they work correctly.
		 */
		if (tty->term->flags & TERM_256COLOURS) {
			if (*type == '3') {
				if (!tty_term_has(tty->term, TTYC_SETAF))
					goto fallback_256;
				tty_putcode1(tty, TTYC_SETAF, colour & 0xff);
			} else {
				if (!tty_term_has(tty->term, TTYC_SETAB))
					goto fallback_256;
				tty_putcode1(tty, TTYC_SETAB, colour & 0xff);
			}
			return (0);
		}
		goto fallback_256;
	}

	if (colour & COLOUR_FLAG_RGB) {
		if (!tty_term_flag(tty->term, TTYC_TC))
			return (-1);

		colour_split_rgb(colour & 0xffffff, &r, &g, &b);
		xsnprintf(s, sizeof s, "\033[%s;2;%hhu;%hhu;%hhum", type,
		    r, g, b);
		tty_puts(tty, s);
		return (0);
	}

	return (-1);

fallback_256:
	xsnprintf(s, sizeof s, "\033[%s;5;%dm", type, colour & 0xff);
	tty_puts(tty, s);
	return (0);
}

void
tty_default_colours(struct grid_cell *gc, const struct window_pane *wp)
{
	struct window		*w = wp->window;
	struct options		*oo = w->options;
	const struct grid_cell	*agc, *pgc, *wgc;

	if (w->flags & WINDOW_STYLECHANGED) {
		w->flags &= ~WINDOW_STYLECHANGED;
		agc = options_get_style(oo, "window-active-style");
		memcpy(&w->active_style, agc, sizeof w->active_style);
		wgc = options_get_style(oo, "window-style");
		memcpy(&w->style, wgc, sizeof w->style);
	} else {
		agc = &w->active_style;
		wgc = &w->style;
	}
	pgc = &wp->colgc;

	if (gc->fg == 8) {
		if (pgc->fg != 8)
			gc->fg = pgc->fg;
		else if (wp == w->active && agc->fg != 8)
			gc->fg = agc->fg;
		else
			gc->fg = wgc->fg;
	}

	if (gc->bg == 8) {
		if (pgc->bg != 8)
			gc->bg = pgc->bg;
		else if (wp == w->active && agc->bg != 8)
			gc->bg = agc->bg;
		else
			gc->bg = wgc->bg;
	}
}