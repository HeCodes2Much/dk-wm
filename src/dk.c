/* dk - /dəˈkā/ window manager
 *
 * see license file for copyright and license details
 * vim:ft=c:fdm=syntax:ts=4:sts=4:sw=4
 */

#define USES_XCB_CONNECTION
#define _XOPEN_SOURCE 700

#include <sys/un.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <signal.h>
#include <err.h>

#include <xcb/randr.h>
#include <xcb/xproto.h>
#include <xcb/xcb_util.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_keysyms.h>


#include "dk.h"
#include "strl.h"
#include "util.h"
#include "parse.h"
#include "layout.h"
#include "event.h"
#include "cmd.h"

#include "config.h"


FILE *cmdresp;
char *argv0, *sock = NULL;
unsigned int lockmask = 0;
int scr_h, scr_w, sockfd, randrbase, cmdusemon;
int running, restart, needsrefresh, status_usingcmdresp;

Desk *desks;
Rule *rules;
Panel *panels;
Status *stats;
Client *cmdclient;
Monitor *primary, *monitors, *selmon, *lastmon;
Workspace *setws, *selws, *lastws, *workspaces;

xcb_screen_t *scr;
xcb_connection_t *con;
xcb_window_t root, wmcheck;
xcb_key_symbols_t *keysyms;
xcb_cursor_t cursor[CURS_LAST];
xcb_atom_t wmatom[WM_LAST], netatom[NET_LAST];

const char *ebadarg = "invalid argument for";
const char *enoargs = "command requires additional arguments but none were given";
const char *gravities[] = {
	[GRAV_NONE] = "none",     [GRAV_LEFT] = "left", [GRAV_RIGHT] = "right",
	[GRAV_CENTER] = "center", [GRAV_TOP] = "top",   [GRAV_BOTTOM] = "bottom",
};
const char *wmatoms[] = {
	[WM_DELETE] = "WM_DELETE_WINDOW", [WM_FOCUS] = "WM_TAKE_FOCUS",
	[WM_MOTIF] = "_MOTIF_WM_HINTS",   [WM_PROTO] = "WM_PROTOCOLS",
	[WM_STATE] = "WM_STATE",          [WM_UTF8STR] = "UTF8_STRING",
};
const char *netatoms[] = {
	[NET_ACTIVE] = "_NET_ACTIVE_WINDOW",
	[NET_CLIENTS] = "_NET_CLIENT_LIST",
	[NET_CLOSE] = "_NET_CLOSE_WINDOW",
	[NET_DESK_CUR] = "_NET_CURRENT_DESKTOP",
	[NET_DESK_GEOM] = "_NET_DESKTOP_GEOMETRY",
	[NET_DESK_NAMES] = "_NET_DESKTOP_NAMES",
	[NET_DESK_NUM] = "_NET_NUMBER_OF_DESKTOPS",
	[NET_DESK_VP] = "_NET_DESKTOP_VIEWPORT",
	[NET_DESK_WA] = "_NET_WORKAREA",
	[NET_STATE_FULL] = "_NET_WM_STATE_FULLSCREEN",
	[NET_SUPPORTED] = "_NET_SUPPORTED",
	[NET_TYPE_DESK] = "_NET_WM_WINDOW_TYPE_DESKTOP",
	[NET_TYPE_DIALOG] = "_NET_WM_WINDOW_TYPE_DIALOG",
	[NET_TYPE_DOCK] = "_NET_WM_WINDOW_TYPE_DOCK",
	[NET_TYPE_SPLASH] = "_NET_WM_WINDOW_TYPE_SPLASH",
	[NET_WM_CHECK] = "_NET_SUPPORTING_WM_CHECK",
	[NET_WM_DESK] = "_NET_WM_DESKTOP",
	[NET_WM_NAME] = "_NET_WM_NAME",
	[NET_WM_STATE] = "_NET_WM_STATE",
	[NET_WM_STRUTP] = "_NET_WM_STRUT_PARTIAL",
	[NET_WM_STRUT] = "_NET_WM_STRUT",
	[NET_WM_TYPE] = "_NET_WM_WINDOW_TYPE",
};


int main(int argc, char *argv[])
{
	ssize_t n;
	Client *c = NULL;
	Status *s, *next;
	fd_set read_fds;
	xcb_window_t sel;
	xcb_generic_event_t *ev;
	char *end, buf[PIPE_BUF];
	int cmdfd, confd, nfds;

	argv0 = argv[0];
	randrbase = -1;
	running = needsrefresh = 1;
	sockfd = restart = cmdusemon = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s")) {
			if (!(sockfd = strtol(argv[++i], &end, 0)) || *end != '\0') {
				warnx("invalid socket file descriptor: %s", argv[i]);
				sockfd = 0;
			}
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-h")) {
			return usage(argv[0], VERSION, 0, argv[i][1], "[-hv] [-s SOCKET_FD]");
		} else {
			return usage(argv[0], VERSION, 1, 'h', "[-hv] [-s SOCKET_FD]");
		}
	}
	if (!setlocale(LC_ALL, ""))
		err(1, "no locale support");
	if (xcb_connection_has_error((con = xcb_connect(NULL, NULL))))
		err(1, "error connecting to X");
	atexit(freewm);
	if (!(scr = xcb_setup_roots_iterator(xcb_get_setup(con)).data))
		errx(1, "error getting default screen from X connection");
	root = scr->root;
	scr_w = scr->width_in_pixels;
	scr_h = scr->height_in_pixels;
	iferr(1, "is another window manager running?",
			xcb_request_check(con,
				xcb_change_window_attributes_checked(con, root, XCB_CW_EVENT_MASK,
					(unsigned int[]){XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT})));

#ifdef __OpenBSD__
	if (pledge("stdio rpath wpath cpath tmppath flock unix proc exec", NULL) == -1)
		err(1, "pledge");
#endif

	initwm();
	initsock();
	initscan();
	execcfg();

	if (winprop(root, netatom[NET_ACTIVE], &sel) && (c = wintoclient(sel))) {
		focus(c);
		xcb_warp_pointer(con, root, root, 0, 0, 0, 0, c->x + (c->w / 2), c->y + (c->h / 2));
	} else if (nextmon(monitors->next) && primary) {
		xcb_warp_pointer(con, root, root, 0, 0, 0, 0, primary->x + (primary->w / 2),
				primary->y + (primary->h / 2));
	}

	confd = xcb_get_file_descriptor(con);
	nfds = MAX(confd, sockfd) + 1;
	while (running) {
		xcb_flush(con);
		if (xcb_connection_has_error(con)) break;
		if (needsrefresh) needsrefresh = refresh();
		FD_ZERO(&read_fds);
		FD_SET(sockfd, &read_fds);
		FD_SET(confd, &read_fds);
		if (select(nfds, &read_fds, NULL, NULL, NULL) > 0) {
			if (FD_ISSET(sockfd, &read_fds)) {
				cmdfd = accept(sockfd, NULL, 0);
				if (cmdfd > 0 && (n = recv(cmdfd, buf, sizeof(buf) - 1, 0)) > 0) {
					if (buf[n - 1] == '\n') n--;
					buf[n] = '\0';
					if (!(cmdresp = fdopen(cmdfd, "w"))) {
						warn("unable to open the socket as file: %s", sock);
						close(cmdfd);
					}
					parsecmd(buf);
				}
			}
			if (FD_ISSET(confd, &read_fds))
				while ((ev = xcb_poll_for_event(con))) {
					dispatch(ev);
					free(ev);
				}
		}

		s = stats;
		while (s) {
			next = s->next;
			if (write(fileno(s->file), 0, 0) == -1) freestatus(s);
			s = next;
		}
	}
	return 0;
}

void applypanelstrut(Panel *p)
{
	DBGENTER("applypanelstrut")
	if (p->mon->x + p->l > p->mon->wx)          p->mon->wx = p->l;
	if (p->mon->y + p->t > p->mon->wy)          p->mon->wy = p->t;
	if (p->mon->w - (p->r + p->l) < p->mon->ww) p->mon->ww = p->mon->w - (p->r + p->l);
	if (p->mon->h - (p->b + p->t) < p->mon->wh) p->mon->wh = p->mon->h - (p->b + p->t);
	DBG("applypanelstrut: %s - %d,%d @ %dx%d -> %d,%d @ %dx%d",
			p->mon->name, p->mon->x, p->mon->y, p->mon->w, p->mon->h,
			p->mon->wx, p->mon->wy, p->mon->ww, p->mon->wh)
	DBGEXIT("applypanelstrut")
}

int applysizehints(Client *c, int *x, int *y, int *w, int *h, int bw, int usermotion, int mouse)
{
	DBGENTER("applysizehints")
	Monitor *m = c->ws->mon;
	int min = globalcfg[GLB_MIN_XY];

	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (usermotion) {
		if (!mouse) {
			if (*w > c->w && c->inc_w > *w - c->w)
				*w = c->w + c->inc_w;
			else if (*w < c->w && c->inc_w > c->w - *w)
				*w = c->w - c->inc_w;
			if (*h > c->h && c->inc_h > *h - c->h)
				*h = c->h + c->inc_h;
			else if (*h < c->h && c->inc_h > c->h - *h)
				*h = c->h - c->inc_h;
			*h = MIN(*h, m->wh);
			*w = MIN(*w, m->ww);
		}
		*x = CLAMP(*x, (*w - min) * -1, scr_w - min);
		*y = CLAMP(*y, (*h - min) * -1, scr_h - min);
	} else {
		*x = CLAMP(*x, m->wx, m->wx + m->ww - *w + (2 * bw));
		*y = CLAMP(*y, m->wy, m->wy + m->wh - *h + (2 * bw));
	}

	if (FLOATING(c) || globalcfg[GLB_TILE_HINTS]) {
		int baseismin;
		if (!(baseismin = (c->base_w == c->min_w && c->base_h == c->min_h)))
			*w -= c->base_w, *h -= c->base_h;
		if (c->min_aspect > 0 && c->max_aspect > 0) {
			if (c->max_aspect < (float)*w / *h)
				*w = *h * c->max_aspect + 0.5;
			else if (c->min_aspect < (float)*h / *w)
				*h = *w * c->min_aspect + 0.5;
		}
		if (baseismin)
			*w -= c->base_w, *h -= c->base_h;
		if (c->inc_w) *w -= *w % c->inc_w;
		if (c->inc_h) *h -= *h % c->inc_h;
		*w += c->base_w;
		*h += c->base_h;
		*w = MAX(*w, c->min_w);
		*h = MAX(*h, c->min_h);
		if (c->max_w) *w = MIN(*w, c->max_w);
		if (c->max_h) *h = MIN(*h, c->max_h);
	}
	DBGEXIT("applysizehints")
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h || bw != c->bw;
}

void attach(Client *c, int tohead)
{
	DBGENTER("attach")
	Client *tail = NULL;

	if (!tohead)
		FIND_TAIL(tail, c->ws->clients);
	if (tail)
		ATTACH(c, tail->next);
	else
		ATTACH(c, c->ws->clients);
	DBGEXIT("attach")
}

int assignws(Workspace *ws, Monitor *new)
{
	DBGENTER("assignws")
	int n;
	Workspace *ows;

	if (ws->mon == new) return 1;
	DBG("assignws: ws: %d -> new mon: %s", ws->num, new->name)
	for (n = 0, ows = workspaces; ows; ows = ows->next) {
		if (ows->mon == ws->mon) {
			n++;
			if (ows != ws) {
				n++;
				break;
			}
		}
	}
	if (n > 1 && ows) {
		DBG("assignws: old mon: %s has available workspace: %d", ws->mon->name, ows->num)
		if (ws == ws->mon->ws)
			ws->mon->ws = ows;
		Monitor *old = ws->mon;
		ws->mon = new;
		relocatews(ws, old);
		needsrefresh = 1;
	} else {
		respond(cmdresp, "!unable to assign last/only workspace on monitor");
		return 0;
	}
	DBGEXIT("assignws")
	return 1;
}

void changews(Workspace *ws, int swap, int warp)
{
	DBGENTER("changews")
	Monitor *m;
	int dowarp;

	if (!ws || ws == selws) return;
	DBG("changews: %d:%s -> %d:%s - swap: %d - warp: %d",
			selws->num, selws->mon->name, ws->num, ws->mon->name, swap, warp)
	dowarp = !swap && warp && selws->mon != ws->mon;
	lastws = selws;
	lastmon = selmon;
	m = selws->mon;
	if (selws->sel)
		unfocus(selws->sel, 1);
	if (swap && m != ws->mon) {
		Monitor *old = ws->mon;
		selws->mon = ws->mon;
		if (ws->mon->ws == ws)
			ws->mon->ws = selws;
		ws->mon = m;
		m->ws = ws;
		updnetworkspaces();
		relocatews(ws, old);
		if (lastws->mon->ws == lastws)
			relocatews(lastws, selmon);
	}
	selws = ws;
	selmon = selws->mon;
	selmon->ws = selws;
	if (dowarp)
		xcb_warp_pointer(con, root, root, 0, 0, 0, 0,
				ws->sel ? ws->sel->x + (ws->sel->w / 2) : ws->mon->x + (ws->mon->w / 2),
				ws->sel ? ws->sel->y + (ws->sel->h / 2) : ws->mon->y + (ws->mon->h / 2));
	PROP(REPLACE, root, netatom[NET_DESK_CUR], XCB_ATOM_CARDINAL, 32, 1, &ws->num);
	needsrefresh = 1;
	DBGEXIT("changews")
}

void clienthints(Client *c)
{
	DBGENTER("clienthints")
	xcb_generic_error_t *e;
	xcb_icccm_wm_hints_t wmh;

	if (xcb_icccm_get_wm_hints_reply(con, xcb_icccm_get_wm_hints(con, c->win), &wmh, &e)) {
		if (c == selws->sel && wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY) {
			wmh.flags &= ~XCB_ICCCM_WM_HINT_X_URGENCY;
			xcb_icccm_set_wm_hints(con, c->win, &wmh);
		} else if (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY) {
			c->state |= STATE_URGENT;
		}
		if ((wmh.flags & XCB_ICCCM_WM_HINT_INPUT) && !wmh.input)
			c->state |= STATE_NOINPUT;
	} else {
		iferr(0, "unable to get window wm hints reply", e);
	}
	DBGEXIT("clienthints")
}

int clientname(Client *c)
{
	DBGENTER("clientname")
	xcb_generic_error_t *e;
	xcb_icccm_get_text_property_reply_t r;

	if (!xcb_icccm_get_text_property_reply(con,
				xcb_icccm_get_text_property(con, c->win, netatom[NET_WM_NAME]), &r, &e))
	{
		iferr(0, "unable to get NET_WM_NAME text property reply", e);
		if (!xcb_icccm_get_text_property_reply(con,
					xcb_icccm_get_text_property(con, c->win, XCB_ATOM_WM_NAME), &r, &e))
		{
			iferr(0, "unable to get WM_NAME text property reply", e);
			strlcpy(c->title, "broken", sizeof(c->title));
			DBGEXIT("clientname")
			return 0;
		}
	}
	strlcpy(c->title, r.name, sizeof(c->title));
	xcb_icccm_get_text_property_reply_wipe(&r);
	DBGEXIT("clientname")
	return 1;
}

void clientrule(Client *c, Rule *wr, int nofocus)
{
	DBGENTER("clientrule")
	Monitor *m;
	Rule *r = wr;
	int ws, dofocus = 0;
	xcb_atom_t cur = selws->num;

	if (c->trans)
		cur = c->trans->ws->num;
	else if (!winprop(c->win, netatom[NET_WM_DESK], &cur) || cur > 99)
		cur = selws->num;
	ws = cur;

	if (!r) {
		for (r = rules; r; r = r->next)
			if (rulecmp(c, r)) break;
	} else if (!rulecmp(c, r)) {
		r = NULL;
	}
	if (r) {
		DBG("clientrule: matched: %s, %s, %s", r->class, r->inst, r->title)
		c->cb = r->cb;
		dofocus = r->focus;
		c->state |= r->state;
		c->x = r->x != -1 ? r->x : c->x;
		c->y = r->y != -1 ? r->y : c->y;
		c->w = r->w != -1 ? r->w : c->w;
		c->h = r->h != -1 ? r->h : c->h;
		c->bw = r->bw != -1 && !(c->state & STATE_NOBORDER) ? r->bw : c->bw;
		if (!c->trans) {
			if ((cmdusemon = (r->mon != NULL))) {
				int num;
				if ((num = strtol(r->mon, NULL, 0)) > 0 && (m = itomon(num))) {
					ws = m->ws->num;
				} else for (m = monitors; m; m = m->next) {
					if (!strcmp(r->mon, m->name)) {
						ws = m->ws->num;
						break;
					}
				}
			} else if (r->ws > 0 && r->ws <= globalcfg[GLB_WS_NUM]) {
				ws = r->ws - 1;
			}
		}
	}

	if (ws + 1 > globalcfg[GLB_WS_NUM] && ws <= 99)
		updworkspaces(ws + 1);
	setworkspace(c, MIN(ws, globalcfg[GLB_WS_NUM]), nofocus);
	if (dofocus && c->ws != selws)
		cmdview(c->ws);
	if (r)
		gravitate(c, r->xgrav, r->ygrav, 1);
	cmdusemon = 0;
	DBGEXIT("clientrule")
}

void clienttype(Client *c)
{
	DBGENTER("clienttype")
	xcb_atom_t type, state;

	if (winprop(c->win, netatom[NET_WM_STATE], &state) && state == netatom[NET_STATE_FULL])
		setfullscreen(c, 1);
	if ((winprop(c->win, netatom[NET_WM_TYPE], &type) && type == netatom[NET_TYPE_DIALOG])
			|| c->trans || (c->trans = wintoclient(wintrans(c->win))))
		c->state |= STATE_FLOATING;
	DBGEXIT("clienttype")
}

void drawborder(Client *c, int focused)
{ /* modified from swm/wmutils */
	DBGENTER("drawborder")
	xcb_gcontext_t gc;
	xcb_pixmap_t pmap;
	int b = c->bw;
	int o = border[BORD_O_WIDTH];
	unsigned int in = border[focused ? BORD_FOCUS : ((c->state & STATE_URGENT)
			? BORD_URGENT : BORD_UNFOCUS)];

	if (c->state & STATE_NOBORDER || !c->bw) return;
	if (b - o > 0) {
		unsigned int out = border[focused ? BORD_O_FOCUS : ((c->state & STATE_URGENT)
				? BORD_O_URGENT : BORD_O_UNFOCUS)];
		xcb_rectangle_t inner[] = {
			{ c->w,         0,            b - o,        c->h + b - o },
			{ c->w + b + o, 0,            b - o,        c->h + b - o },
			{ 0,            c->h,         c->w + b - o, b - o        },
			{ 0,            c->h + b + o, c->w + b - o, b - o        },
			{ c->w + b + o, c->h + b + o, b,            b            }
		};
		xcb_rectangle_t outer[] = {
			{ c->w + b - o, 0,            o,            c->h + b * 2 },
			{ c->w + b,     0,            o,            c->h + b * 2 },
			{ 0,            c->h + b - o, c->w + b * 2, o            },
			{ 0,            c->h + b,     c->w + b * 2, o            },
			{ 1,            1,            1,            1            }
		};

		pmap = xcb_generate_id(con);
		xcb_create_pixmap(con, c->depth, pmap, c->win, W(c), H(c));
		gc = xcb_generate_id(con);
		xcb_create_gc(con, gc, pmap, XCB_GC_FOREGROUND, &in);
		xcb_poly_fill_rectangle(con, pmap, gc, LEN(inner), inner);
		xcb_change_gc(con, gc, XCB_GC_FOREGROUND, &out);
		xcb_poly_fill_rectangle(con, pmap, gc, LEN(outer), outer);
		xcb_change_window_attributes(con, c->win, XCB_CW_BORDER_PIXMAP, &pmap);
		xcb_free_pixmap(con, pmap);
		xcb_free_gc(con, gc);
	} else {
		xcb_change_window_attributes(con, c->win, XCB_CW_BORDER_PIXEL, &in);
	}
	DBGEXIT("drawborder")
}

Monitor *coordtomon(int x, int y)
{
	DBGENTER("coordtomon")
	Monitor *m = NULL;

	FOR_EACH(m, monitors)
		if (m->connected && x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h)
			break;
	DBGEXIT("coordtomon")
	return m;
}

void detach(Client *c, int reattach)
{
	DBGENTER("detach")
	Client **cc = &c->ws->clients;

	DETACH(c, cc);
	if (reattach)
		ATTACH(c, c->ws->clients);
	DBGEXIT("detach")
}

void detachstack(Client *c)
{
	DBGENTER("detachstack")
	Client **cc = &c->ws->stack;

	while (*cc && *cc != c)
		cc = &(*cc)->snext;
	*cc = c->snext;
	if (c == c->ws->sel)
		c->ws->sel = c->ws->stack;
	DBGEXIT("detachstack")
}

void execcfg(void)
{
	DBGENTER("execcfg")
	char *cfg;
	char path[PATH_MAX];

	if (!(cfg = getenv("DKRC"))) {
		char *s = getenv("HOME");
		if (!s) {
			warn("getenv");
			return;
		}
		strlcpy(path, s, sizeof(path));
		strlcat(path, "/.config/dk/dkrc", sizeof(path));
		cfg = path;
	}
	if (!fork()) {
		if (con)
			close(xcb_get_file_descriptor(con));
		setsid();
		execle(cfg, cfg, (char *)NULL, environ);
		warn("unable to execute config file: %s", cfg);
		exit(0);
	}
	DBGEXIT("execcfg")
}

void focus(Client *c)
{
	DBGENTER("focus")
	if (!c)
		c = selws->stack;
	if (selws->sel && selws->sel != c)
		unfocus(selws->sel, 0);
	if (c) {
		if (c->state & STATE_URGENT)
			seturgent(c, 0);
		detachstack(c);
		c->snext = c->ws->stack;
		c->ws->stack = c;
		grabbuttons(c, 1);
		drawborder(c, 1);
		setinputfocus(c);
	} else {
		unfocus(c, 1);
	}
	selws->sel = c;
	cmdclient = c;
	DBGEXIT("focus")
}

void freemon(Monitor *m)
{
	DBGENTER("freemon")
	Monitor **mm = &monitors;

	DETACH(m, mm);
	free(m);
	DBGEXIT("freemon")
}

void freerule(Rule *r)
{
	DBGENTER("freerule")
	Rule **rr = &rules;

	DETACH(r, rr);
	if (r->class) { regfree(&(r->classreg)); free(r->class); }
	if (r->inst) { regfree(&(r->instreg)); free(r->inst); }
	if (r->title) { regfree(&(r->titlereg)); free(r->title); }
	free(r->mon);
	free(r);
	DBGEXIT("freerule")
}

void freestatus(Status *s)
{
	DBGENTER("freestatus")
	Status **ss = &stats;

	DBG("freestatus: path: %s", s->path)
	DETACH(s, ss);
	if (!restart) fclose(s->file);
	if (s->path) free(s->path);
	free(s);
	DBGEXIT("freestatus")
}

void freewm(void)
{
	DBGENTER("freewm")
	Client *c;
	Workspace *ws;

	while (panels) unmanage(panels->win, 0);
	while (desks) unmanage(desks->win, 0);

	if (!restart) /* move clients back into visible space when not restarting */
		FOR_CLIENTS(c, ws)
			MOVE(c->win, c->x, c->y);
	while (workspaces) {
		while (workspaces->stack)
			unmanage(workspaces->stack->win, 0);
		freews(workspaces);
	}
	while (monitors) freemon(monitors);
	while (rules) freerule(rules);
	while (stats) freestatus(stats);

	xcb_key_symbols_free(keysyms);
	for (unsigned int i = 0; i < LEN(cursors); i++)
		xcb_free_cursor(con, cursor[i]);
	xcb_destroy_window(con, wmcheck);
	xcb_set_input_focus(con, XCB_INPUT_FOCUS_POINTER_ROOT,
			XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);

	if (!restart)
		xcb_delete_property(con, root, netatom[NET_ACTIVE]);
	xcb_aux_sync(con);
	xcb_disconnect(con);

	if (restart) {
		char fdstr[64];
		if (!itoa(sockfd, fdstr))
			itoa(-1, fdstr);
		char *const arg[] = { argv0, "-s", fdstr, NULL };
		execvp(arg[0], arg);
	}

	close(sockfd);
	unlink(sock);
	free(sock);
	DBGEXIT("freewm")
}

void freews(Workspace *ws)
{
	DBGENTER("freews")
	Workspace **wws = &workspaces;

	DETACH(ws, wws);
	free(ws);
	DBGEXIT("freews")
}

void grabbuttons(Client *c, int focused)
{
	DBGENTER("grabbuttons")
	xcb_generic_error_t *e;
	xcb_get_modifier_mapping_reply_t *m = NULL;
	unsigned int mods[] = { 0, XCB_MOD_MASK_LOCK, 0, XCB_MOD_MASK_LOCK };

	lockmask = 0;
	if ((m = xcb_get_modifier_mapping_reply(con, xcb_get_modifier_mapping(con), &e))) {
		xcb_keycode_t *k, *t = NULL;
		if ((t = xcb_key_symbols_get_keycode(keysyms, 0xff7f))
				&& (k = xcb_get_modifier_mapping_keycodes(m)))
		{
			for (unsigned int i = 0; i < 8; i++)
				for (unsigned int j = 0; j < m->keycodes_per_modifier; j++)
					if (k[i * m->keycodes_per_modifier + j] == *t)
						lockmask = (1 << i);
		}
		free(t);
	} else {
		iferr(0, "unable to get modifier mapping for numlock", e);
	}
	free(m);

	mods[2] |= lockmask, mods[3] |= lockmask;
	xcb_ungrab_button(con, XCB_BUTTON_INDEX_ANY, c->win, XCB_BUTTON_MASK_ANY);
	if (!focused)
		xcb_grab_button(con, 0, c->win,
				XCB_EVENT_MASK_BUTTON_PRESS,
				XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC, XCB_NONE, XCB_NONE,
				XCB_BUTTON_INDEX_ANY, XCB_BUTTON_MASK_ANY);
	else for (unsigned int i = 0; i < LEN(mods); i++) {
			xcb_grab_button(con, 0, c->win,
					XCB_EVENT_MASK_BUTTON_PRESS,
					XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_SYNC, XCB_NONE, XCB_NONE,
					mousemove, mousemod | mods[i]);
			xcb_grab_button(con, 0, c->win,
					XCB_EVENT_MASK_BUTTON_PRESS,
					XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_SYNC, XCB_NONE, XCB_NONE,
					mouseresize, mousemod | mods[i]);
		}
	DBGEXIT("grabbuttons")
}

void gravitate(Client *c, int xgrav, int ygrav, int matchgap)
{
	DBGENTER("gravitate")
	int x, y, gap;
	int monx, mony, monw, monh;

	if (!c || !c->ws || !FLOATING(c)) return;
	x = c->x, y = c->y;
	if (c->trans) {
		gap = 0;
		monx = c->trans->x, mony = c->trans->y;
		monw = c->trans->w, monh = c->trans->h;
	} else {
		gap = matchgap ? c->ws->gappx : 0;
		monx = c->ws->mon->wx, mony = c->ws->mon->wy;
		monw = c->ws->mon->ww, monh = c->ws->mon->wh;
	}
	switch (xgrav) {
	case GRAV_LEFT:   x = monx + gap; break;
	case GRAV_RIGHT:  x = monx + monw - W(c) - gap; break;
	case GRAV_CENTER: x = (monx + monw - W(c)) / 2; break;
	}
	switch (ygrav) {
	case GRAV_TOP:    y = mony + gap; break;
	case GRAV_BOTTOM: y = mony + monh - H(c) - gap; break;
	case GRAV_CENTER: y = (mony + monh - H(c)) / 2; break;
	}
	if (c->ws == c->ws->mon->ws && x != c->x && y != c->y)
		resizehint(c, x, y, c->w, c->h, c->bw, 0, 0);
	DBGEXIT("gravitate")
}

int iferr(int lvl, char *msg, xcb_generic_error_t *e)
{
	DBGENTER("iferr")
	if (!e) return 1;
	warn("%s", msg);
	free(e);
	if (lvl) exit(lvl);
	return 0;
}

void initatoms(xcb_atom_t *atoms, const char **names, int num)
{
	DBGENTER("initatoms")
	int i;
	xcb_generic_error_t *e;
	xcb_intern_atom_reply_t *r;
	xcb_intern_atom_cookie_t c[num];

	for (i = 0; i < num; ++i)
		c[i] = xcb_intern_atom(con, 0, strlen(names[i]), names[i]);
	for (i = 0; i < num; ++i) {
		if ((r = xcb_intern_atom_reply(con, c[i], &e))) {
			atoms[i] = r->atom;
			free(r);
		} else {
			iferr(0, "unable to initialize atom", e);
		}
	}
	DBGEXIT("initatoms")
}

void initclient(xcb_window_t win, xcb_get_geometry_reply_t *g)
{
	DBGENTER("initclient")
	Client *c;
	xcb_generic_error_t *e;
	xcb_get_property_cookie_t pc;
	xcb_get_property_reply_t *pr;
	xcb_icccm_get_wm_class_reply_t p;

	DBG("initclient: 0x%08x", win)
	c = ecalloc(1, sizeof(Client));
	c->win = win;
	c->depth = g->depth;
	c->x = c->old_x = g->x;
	c->y = c->old_y = g->y;
	c->w = c->old_w = g->width;
	c->h = c->old_h = g->height;
	c->bw = c->old_bw = border[BORD_WIDTH];
	c->state = STATE_NEEDSMAP;
	c->old_state = STATE_NONE;
	c->trans = wintoclient(wintrans(c->win));
	clientname(c);
	if (!xcb_icccm_get_wm_class_reply(con, xcb_icccm_get_wm_class(con, c->win), &p, &e)) {
		iferr(0, "failed to get window class", e);
		strlcpy(c->class, "broken", sizeof(c->class));
		strlcpy(c->inst, "broken", sizeof(c->inst));
	} else {
		strlcpy(c->class, p.class_name, sizeof(c->class));
		strlcpy(c->inst, p.instance_name, sizeof(c->inst));
		xcb_icccm_get_wm_class_reply_wipe(&p);
	}

	pc = xcb_get_property(con, 0, c->win, wmatom[WM_MOTIF], wmatom[WM_MOTIF], 0, 5);
	if ((pr = xcb_get_property_reply(con, pc, &e))) {
		if (((xcb_atom_t *)xcb_get_property_value(pr))[2] == 0) {
			c->bw = 0;
			c->state |= STATE_NOBORDER;
		}
	} else {
		iferr(0, "unable to get window motif hints reply", e);
	}
	free(pr);

	clientrule(c, NULL, !globalcfg[GLB_FOCUS_OPEN]);
	clienttype(c);
	sizehints(c, 1);
	clienthints(c);
	xcb_change_window_attributes(con, c->win, XCB_CW_EVENT_MASK,
			(unsigned int[]){ XCB_EVENT_MASK_ENTER_WINDOW
							| XCB_EVENT_MASK_FOCUS_CHANGE
							| XCB_EVENT_MASK_PROPERTY_CHANGE
							| XCB_EVENT_MASK_STRUCTURE_NOTIFY });
	grabbuttons(c, 0);
	if (FLOATING(c) || c->state & STATE_FIXED) {
		c->w = CLAMP(c->w, globalcfg[GLB_MIN_WH], c->ws->mon->ww);
		c->h = CLAMP(c->h, globalcfg[GLB_MIN_WH], c->ws->mon->wh);
		if (c->trans) {
			c->x = c->trans->x + ((W(c->trans) - W(c)) / 2);
			c->y = c->trans->y + ((H(c->trans) - H(c)) / 2);
		} else {
			c->x = CLAMP(c->x, c->ws->mon->wx, c->ws->mon->wx + c->ws->mon->ww - W(c));
			c->y = CLAMP(c->y, c->ws->mon->wy, c->ws->mon->wy + c->ws->mon->wh - H(c));
		}
		if (c->x == c->ws->mon->wx && c->y == c->ws->mon->wy)
			quadrant(c, &c->x, &c->y, &c->w, &c->h);
	}
	if (c->cb) c->cb->func(c, 0);
	DBGEXIT("initclient")
}

void initdesk(xcb_window_t win, xcb_get_geometry_reply_t *g)
{
	DBGENTER("initdesk")
	Desk *d;

	DBG("initdesk: 0x%08x", win)
	d = ecalloc(1, sizeof(Desk));
	d->win = win;
	if (!(d->mon = coordtomon(g->x, g->y)))
		d->mon = selws->mon;
	d->state |= STATE_NEEDSMAP;
	ATTACH(d, desks);
	xcb_change_window_attributes(con, d->win, XCB_CW_EVENT_MASK,
			(unsigned int[]){ XCB_EVENT_MASK_PROPERTY_CHANGE
							| XCB_EVENT_MASK_STRUCTURE_NOTIFY });
	setstackmode(d->win, XCB_STACK_MODE_BELOW);
	DBGEXIT("initdesk")
}

void initmon(int num, char *name, xcb_randr_output_t id, int x, int y, int w, int h)
{
	DBGENTER("initmon")
	Monitor *m, *tail;

	DBG("initmon: %d:%s - %d,%d @ %dx%d", num, name, x, y, w, h)
	m = ecalloc(1, sizeof(Monitor));
	m->id = id;
	m->num = num;
	m->connected = 1;
	m->x = m->wx = x;
	m->y = m->wy = y;
	m->w = m->ww = w;
	m->h = m->wh = h;
	strlcpy(m->name, name, sizeof(m->name));
	FIND_TAIL(tail, monitors);
	if (tail)
		tail->next = m;
	else
		monitors = m;
	DBGEXIT("initmon")
}

void initpanel(xcb_window_t win, xcb_get_geometry_reply_t *g)
{
	DBGENTER("initpanel")
	int *s;
	Panel *p;
	xcb_generic_error_t *e;
	xcb_get_property_cookie_t rc;
	xcb_get_property_reply_t *prop = NULL;

	DBG("initpanel: 0x%08x", win)
	rc = xcb_get_property(con, 0, win, netatom[NET_WM_STRUTP], XCB_ATOM_CARDINAL, 0, 4);
	p = ecalloc(1, sizeof(Panel));
	p->win = win;
	p->state |= STATE_NEEDSMAP;
	if (!(p->mon = coordtomon(g->x, g->y)))
		p->mon = selws->mon;
	if (!(prop = xcb_get_property_reply(con, rc, &e)) || prop->type == XCB_NONE) {
		iferr(0, "unable to get _NET_WM_STRUT_PARTIAL reply from window", e);
		rc = xcb_get_property(con, 0, p->win, netatom[NET_WM_STRUT], XCB_ATOM_CARDINAL, 0, 4);
		if (!(prop = xcb_get_property_reply(con, rc, &e)))
			iferr(0, "unable to get _NET_WM_STRUT reply from window", e);
	}
	if (prop && prop->value_len && (s = xcb_get_property_value(prop))) {
		DBG("initpanel: 0x%08x - struts: %d, %d, %d, %d", p->win, s[0], s[1], s[2], s[3])
		p->l = s[0];
		p->r = s[1];
		p->t = s[2];
		p->b = s[3];
		updstruts(p, 1);
	}
	free(prop);
	ATTACH(p, panels);
	xcb_change_window_attributes(con, p->win, XCB_CW_EVENT_MASK,
			(unsigned int[]){ XCB_EVENT_MASK_PROPERTY_CHANGE
							| XCB_EVENT_MASK_STRUCTURE_NOTIFY });
	DBGEXIT("initpanel")
}

Rule *initrule(Rule *wr)
{
	DBGENTER("initrule")
	int i;
	Rule *r;
	size_t len;
	char buf[NAME_MAX];

#define CPYSTR(dst, src)                           \
		dst = ecalloc(1, (len = strlen(src) + 1)); \
		strlcpy(dst, src, len)
#define INITREG(str, reg)                                            \
	if ((i = regcomp(reg, str, REG_NOSUB|REG_EXTENDED|REG_ICASE))) { \
		regerror(i, reg, buf, sizeof(buf));                          \
		respond(cmdresp, "!invalid regex %s: %s", str, buf);         \
		goto error;                                                  \
	}
#define FREEREG(str, wstr, reg) if (wstr) { regfree(reg); free(str); }

	r = ecalloc(1, sizeof(Rule));
	memcpy(r, wr, sizeof(Rule)); // NOLINT
	if (wr->mon) { CPYSTR(r->mon, wr->mon); }
	if (wr->title) { CPYSTR(r->title, wr->title); INITREG(r->title, &(r->titlereg)) }
	if (wr->class) { CPYSTR(r->class, wr->class); INITREG(r->class, &(r->classreg)) }
	if (wr->inst) { CPYSTR(r->inst, wr->inst); INITREG(r->inst, &(r->instreg)) }
	ATTACH(r, rules);
	DBGEXIT("initrule (SUCCESS)")
	return r;

error:
	FREEREG(r->title, wr->title, &(r->titlereg))
	FREEREG(r->class, wr->class, &(r->classreg))
	FREEREG(r->inst, wr->inst, &(r->instreg))
	if (wr->mon) free(r->mon);
	free(r);
	DBGEXIT("initrule (FAIL)")
	return NULL;

#undef INITREG
#undef FREEREG
#undef CPYSTR
}

void initscan(void)
{
	DBGENTER("initscan")
	unsigned int i;
	xcb_generic_error_t *e;
	xcb_query_tree_reply_t *rt;

	if (!(rt = xcb_query_tree_reply(con, xcb_query_tree(con, root), &e))) {
		iferr(1, "unable to query tree from root window", e);
	} else if (rt->children_len) {
		xcb_window_t *w = xcb_query_tree_children(rt);
		for (i = 0; i < rt->children_len; i++)
			if (!wintrans(w[i])) {
				manage(w[i], 1);
				w[i] = XCB_WINDOW_NONE;
			}
		for (i = 0; i < rt->children_len; i++)
			if (w[i] != XCB_WINDOW_NONE)
				manage(w[i], 1);
	}
	free(rt);
	DBGEXIT("initscan")
}

void initsock(void)
{
	DBGENTER("initsock")
	ssize_t len;
	char *hostname = NULL;
	int display = 0, screen = 0;
	static struct sockaddr_un addr;

	if (sockfd > 0) return;
	if (xcb_parse_display(NULL, &hostname, &display, &screen)) {
		len = snprintf(NULL, 0, "/tmp/dk_%s_%i_%i.socket", hostname, display, screen) + 1; // NOLINT
		sock = ecalloc(1, len);
		snprintf(sock, len, "/tmp/dk_%s_%i_%i.socket", hostname, display, screen); // NOLINT
		free(hostname);
	} else {
		sock = ecalloc(1, (len = 15));
		strlcpy(sock, "/tmp/dk.socket", len);
	}
	if (setenv("DKSOCK", sock, 0) < 0)
		warn("unable to set DKSOCK environment variable");
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, sock, sizeof(addr.sun_path));
	check((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)), "unable to create socket");
	unlink(sock); // NOLINT  -- shit linter, this will NEVER be NULL
	check(bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)), "unable to bind socket");
	check(listen(sockfd, SOMAXCONN), "unable to listen on socket");
	DBGEXIT("initsock")
}

Status *initstatus(FILE *file, char *path, int num, unsigned int type)
{
	DBGENTER("initstatus")
	Status *s, *tail;

	DBG("initstatus: path: %s, num: %d, type: %d", path, num, type)

	s = ecalloc(1, sizeof(Status));
	if (path) {
		size_t len = strlen(path) + 1;
		s->path = ecalloc(1, len);
		strlcpy(s->path, path, len);
	}
	s->num = num;
	s->type = type;
	s->file = file;
	FIND_TAIL(tail, stats);
	if (tail)
		tail->next = s;
	else
		stats = s;
	DBGEXIT("initstatus")
	return s;
}

void initwm(void)
{
	DBGENTER("initwm")
	int cws;
	xcb_atom_t r;
	Workspace *ws;
	unsigned int i;
	xcb_cursor_context_t *ctx;
	const xcb_query_extension_reply_t *ext;
	struct sigaction sa;
	int sigs[] = { SIGTERM, SIGINT, SIGHUP, SIGCHLD };

	sa.sa_handler = sighandle;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	for (i = 0; i < LEN(sigs); i++)
		check(sigaction(sigs[i], &sa, NULL), "unable to setup signal handler");
	/* handle/ignore SIGPIPE otherwise write() on broken pipes will crash and burn */
	signal(SIGPIPE, SIG_IGN);

	check(xcb_cursor_context_new(con, scr, &ctx), "unable to create cursor context");
	for (i = 0; i < LEN(cursors); i++)
		cursor[i] = xcb_cursor_load_cursor(ctx, cursors[i]);
	xcb_cursor_context_free(ctx);

	initatoms(wmatom, wmatoms, LEN(wmatoms));
	initatoms(netatom, netatoms, LEN(netatoms));

	if ((ext = xcb_get_extension_data(con, &xcb_randr_id)) && ext->present) {
		randrbase = ext->first_event;
		xcb_randr_select_input(con, root,
				XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
				| XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE
				| XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
				| XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);
		updrandr();
	} else {
		warnx("unable to get randr extension data");
	}
	if (randrbase < 0 || !nextmon(monitors))
		initmon(0, "default", 0, 0, 0, scr_w, scr_h);

	cws = winprop(root, netatom[NET_DESK_CUR], &r) && r < 100 ? r : 0;
	updworkspaces(MAX(cws + 1, globalcfg[GLB_WS_NUM]));
	selws = workspaces;
	selmon = selws->mon;
	changews((ws = itows(cws)) ? ws : workspaces, globalcfg[GLB_WS_STATIC], 1);

	wmcheck = xcb_generate_id(con);
	xcb_create_window(con, XCB_COPY_FROM_PARENT, wmcheck, root, -1, -1, 1, 1, 0,
			XCB_WINDOW_CLASS_INPUT_ONLY, scr->root_visual, 0, NULL);
	PROP(REPLACE, wmcheck, netatom[NET_WM_CHECK], XCB_ATOM_WINDOW, 32, 1, &wmcheck);
	PROP(REPLACE, wmcheck, netatom[NET_WM_NAME], wmatom[WM_UTF8STR], 8, 2, "dk");
	PROP(REPLACE, root, netatom[NET_WM_CHECK], XCB_ATOM_WINDOW, 32, 1, &wmcheck);
	PROP(REPLACE, root, netatom[NET_SUPPORTED], XCB_ATOM_ATOM, 32, LEN(netatom), netatom);
	xcb_delete_property(con, root, netatom[NET_CLIENTS]);

	iferr(1, "unable to change root window event mask or cursor",
			xcb_request_check(con,
				xcb_change_window_attributes_checked(
					con, root, XCB_CW_EVENT_MASK | XCB_CW_CURSOR,
					(unsigned int[]){ XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
									| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
									| XCB_EVENT_MASK_BUTTON_PRESS
									| XCB_EVENT_MASK_POINTER_MOTION
									| XCB_EVENT_MASK_ENTER_WINDOW
									| XCB_EVENT_MASK_LEAVE_WINDOW
									| XCB_EVENT_MASK_STRUCTURE_NOTIFY
									| XCB_EVENT_MASK_PROPERTY_CHANGE,
									cursor[CURS_NORMAL]})));

	if (!(keysyms = xcb_key_symbols_alloc(con)))
		err(1, "unable to get keysyms from X connection");
	DBGEXIT("initwm")
}

Workspace *initws(int num)
{
	DBGENTER("initws")
	Workspace *ws, *tail;

	DBG("initws: %d", num)
	ws = ecalloc(1, sizeof(Workspace));
	ws->num = num;
	itoa(num + 1, ws->name);
	ws->gappx = MAX(0, wsdef.gappx);
	ws->layout = wsdef.layout;
	ws->nmaster = MAX(0, wsdef.nmaster);
	ws->nstack = MAX(0, wsdef.nstack);
	ws->msplit = CLAMP(wsdef.msplit, 0.05, 0.95);
	ws->ssplit = CLAMP(wsdef.ssplit, 0.05, 0.95);
	ws->padl = MAX(0, wsdef.padl);
	ws->padr = MAX(0, wsdef.padr);
	ws->padt = MAX(0, wsdef.padt);
	ws->padb = MAX(0, wsdef.padb);
	FIND_TAIL(tail, workspaces);
	if (tail)
		tail->next = ws;
	else
		workspaces = ws;
	DBGEXIT("initws")
	return ws;
}

Monitor *itomon(int num)
{
	DBGENTER("itomon")
	Monitor *mon = monitors;

	while (mon && mon->num != num)
		mon = mon->next;
	DBGEXIT("itomon")
	return mon;
}

Workspace *itows(int num)
{
	DBGENTER("itows")
	Workspace *ws = workspaces;

	while (ws && ws->num != num)
		ws = ws->next;
	DBGEXIT("itows")
	return ws;
}

void manage(xcb_window_t win, int scan)
{
	DBGENTER("manage")
	xcb_atom_t type, state;

	if (wintoclient(win) || wintopanel(win) || wintodesk(win))
		return;
	xcb_get_geometry_reply_t *g = wingeom(win);
	xcb_get_window_attributes_reply_t *wa = winattr(win);
	if (!wa || !g) goto end;

	DBG("manage: 0x%08x - %d,%d @ %dx%d", win, g->x, g->y, g->width, g->height)
	if (winprop(win, netatom[NET_WM_TYPE], &type)) {
		if (type == netatom[NET_TYPE_DOCK])
			initpanel(win, g);
		else if (type == netatom[NET_TYPE_DESK])
			initdesk(win, g);
		else if (type != netatom[NET_TYPE_SPLASH] && !wa->override_redirect)
			goto client;
	} else if (!wa->override_redirect) {
client:
		if (scan)
			if (!(wa->map_state == XCB_MAP_STATE_VIEWABLE
						|| (winprop(win, wmatom[WM_STATE], &state)
							&& state == XCB_ICCCM_WM_STATE_ICONIC)))
				goto end;
		initclient(win, g);
	}
	PROP(APPEND, root, netatom[NET_CLIENTS], XCB_ATOM_WINDOW, 32, 1, &win);
	setwinstate(win, XCB_ICCCM_WM_STATE_NORMAL);
	needsrefresh = 1;

end:
	free(wa);
	free(g);
	DBGEXIT("manage")
}

void movestack(int direction)
{
	DBGENTER("movestack")
	int i;
	Client *c = cmdclient, *t;

	if (!nexttiled(c->ws->clients->next)) return;
	while (direction) {
		if (direction > 0) {
			detach(c, (t = nexttiled(c->next)) ? 0 : 1);
			if (t)
				ATTACH(c, t->next);
			direction--;
		} else {
			if (c == nexttiled(c->ws->clients)) {
				detach(c, 0);
				attach(c, 0);
			} else {
				for (t = nexttiled(c->ws->clients); t && nexttiled(t->next)
						&& nexttiled(t->next) != c; t = nexttiled(t->next))
					;
				detach(c, (i = (t == nexttiled(c->ws->clients)) ? 1 : 0));
				if (!i) {
					c->next = t;
					FIND_PREV(t, c->next, c->ws->clients);
					t->next = c;
				}
			}
			direction++;
		}
	}
	needsrefresh = 1;
	DBGEXIT("movestack")
}

Monitor *nextmon(Monitor *m)
{
	DBGENTER("nextmon")
	while (m && !m->connected)
		m = m->next;
	DBGEXIT("nextmon")
	return m;
}

Client *nexttiled(Client *c)
{
	DBGENTER("nexttiled")
	while (c && FLOATING(c))
		c = c->next;
	DBGEXIT("nexttiled")
	return c;
}

Monitor *outputtomon(xcb_randr_output_t id)
{
	DBGENTER("outputtomon")
	Monitor *m = NULL;

	FOR_EACH(m, monitors)
		if (m->id == id)
			return m;
	DBGEXIT("outputtomon")
	return m;
}

void popfloat(Client *c)
{
	DBGENTER("popfloat")
	int x, y, w, h;

	c->state |= STATE_FLOATING;
	x = c->x, y = c->y;
	w = CLAMP(c->w, c->ws->mon->ww / 8, c->ws->mon->ww / 3);
	h = CLAMP(c->h, c->ws->mon->wh / 8, c->ws->mon->wh / 3);
	quadrant(c, &x, &y, &w, &h);
	setstackmode(c->win, XCB_STACK_MODE_ABOVE);
	resizehint(c, x, y, w, h, c->bw, 0, 0);
	xcb_aux_sync(con);
	DBGEXIT("popfloat")
}

void printstatus(Status *s)
{
	DBGENTER("printstatus")
	Rule *r;
	Client *c;
	Monitor *m;
	Status *next;
	Workspace *ws;
	int single = 1;

	if (!s) {
		s = stats;
		single = 0;
	}
	while (s) {
		next = s->next;
		switch (s->type) {
		case TYPE_WS:
			fprintf(s->file, "W");
			FOR_EACH(ws, workspaces) {
				char fmt[5] = "i%s:";
				if (ws == selws)
					fmt[0] = ws->clients ? 'A' : 'I';
				else
					fmt[0] = ws->clients ? 'a' : 'i';
				if (!ws->next)
					fmt[3] = '\0';
				fprintf(s->file, fmt, ws->name);
			}
			fprintf(s->file, "\nL%s\nA%s", selws->layout->name, selws->sel ? selws->sel->title : "");
			break;
		case TYPE_FULL:
			fprintf(s->file, "# globals - key: value ...\nnumws: %d\nsmart_border: %d\n"
					"smart_gap: %d\nfocus_urgent: %d\nfocus_mouse: %d\nfocus_open: %d\n"
					"tile_hints: %d\ntile_tohead: %d\ntile_rmaster: %d\n"
					"win_minxy: %d\nwin_minwh: %d",
					globalcfg[GLB_WS_NUM], globalcfg[GLB_SMART_BORDER],
					globalcfg[GLB_SMART_GAP], globalcfg[GLB_FOCUS_URGENT],
					globalcfg[GLB_FOCUS_MOUSE], globalcfg[GLB_FOCUS_OPEN],
					globalcfg[GLB_TILE_HINTS], globalcfg[GLB_TILE_TOHEAD],
					globalcfg[GLB_TILE_RMASTER], globalcfg[GLB_MIN_XY], globalcfg[GLB_MIN_WH]);
			fprintf(s->file, "\n\n# width outer_width focus urgent unfocus "
					"outer_focus outer_urgent outer_unfocus\n"
					"border: %u %u 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
					border[BORD_WIDTH], border[BORD_O_WIDTH],
					border[BORD_FOCUS], border[BORD_URGENT],
					border[BORD_UNFOCUS], border[BORD_O_FOCUS],
					border[BORD_O_URGENT], border[BORD_O_UNFOCUS]);
			fprintf(s->file, "\n\n# number:name:layout ...\nworkspaces:");
			FOR_EACH(ws, workspaces)
				fprintf(s->file, " %s%d:%s:%s", ws == selws ? "*" : "", ws->num + 1, ws->name, ws->layout->name);
			fprintf(s->file, "\n\t# number:name active_window nmaster "
					"nstack msplit ssplit gappx padl padr padt padb");
			FOR_EACH(ws, workspaces)
				fprintf(s->file, "\n\t%d:%s 0x%08x %d %d %0.2f %0.2f %d %d %d %d %d",
						ws->num + 1, ws->name, ws->sel ? ws->sel->win : 0, ws->nmaster, ws->nstack,
						ws->msplit, ws->ssplit, ws->gappx, ws->padl, ws->padr, ws->padt, ws->padb);
			fprintf(s->file, "\n\n# number:name:workspace ...\nmonitors:");
			FOR_EACH(m, monitors)
				if (m->connected)
					fprintf(s->file, " %s%d:%s:%d", m->ws == selws ? "*" : "", m->num + 1, m->name, m->ws->num + 1);
			fprintf(s->file, "\n\t# number:name active_window x y width height wx wy wwidth wheight");
			FOR_EACH(m, monitors)
				if (m->connected)
					fprintf(s->file, "\n\t%d:%s 0x%08x %d %d %d %d %d %d %d %d",
							m->num + 1, m->name, m->ws->sel ? m->ws->sel->win : 0,
							m->x, m->y, m->w, m->h, m->wx, m->wy, m->ww, m->wh);
			fprintf(s->file, "\n\n# id:workspace ...\nwindows:");
			FOR_CLIENTS(c, ws)
				fprintf(s->file, " %s0x%08x:%d", c == selws->sel ? "*" : "", c->win, c->ws->num + 1);
			fprintf(s->file, "\n\t# id title class instance x y width height bw hoff "
					"float full fakefull fixed stick urgent callback trans_id");
			FOR_CLIENTS(c, ws)
				fprintf(s->file, "\n\t0x%08x \"%s\" \"%s\" \"%s\" %d %d %d %d %d %d %d %d %d %d %d %d %s 0x%08x",
						c->win, c->title, c->class, c->inst, c->x, c->y, c->w, c->h, c->bw,
						c->hoff, FLOATING(c), (c->state & STATE_FULLSCREEN) != 0,
						(c->state & STATE_FAKEFULL) != 0, (c->state & STATE_FIXED) != 0,
						(c->state & STATE_STICKY) != 0, (c->state & STATE_URGENT) != 0,
						c->cb ? c->cb->name : "", c->trans ? c->trans->win : 0);
			fprintf(s->file, "\n\n# title class instance workspace monitor float "
					"stick focus callback x y width height xgrav ygrav");
			FOR_EACH(r, rules)
				fprintf(s->file, "\nrule: \"%s\" \"%s\" \"%s\" %d %s %d %d %d %s %d %d %d %d %s %s",
						r->title, r->class, r->inst, r->ws, r->mon, (r->state & STATE_FLOATING) != 0,
						(r->state & STATE_STICKY) != 0, r->focus, r->cb ? r->cb->name : "",
						r->x, r->y, r->w, r->h, gravities[r->xgrav], gravities[r->ygrav]);
			fprintf(s->file, "\n");
			break;
		}
		fflush(s->file);
		if (!(s->num -= s->num > 0 ? 1 : 0))
			freestatus(s);
		if (single) break;
		s = next;
	}
	DBGEXIT("printstatus")
}

void quadrant(Client *c, int *x, int *y, int *w, int *h)
{
	DBGENTER("quadrant")
	Client *t;
	Monitor *m = c->ws->mon;
	static int index = 0;
	static Workspace *ws = NULL;
	unsigned int i = 0;
	int tw = m->ww / 3, th = m->wh / 3;
	int q[][3] = {
		{ 1, m->wx + tw,       m->wy + th },
		{ 1, m->wx + (tw * 2), m->wy + th },
		{ 1, m->wx,            m->wy + th },
		{ 1, m->wx + tw,       m->wy, },
		{ 1, m->wx + (tw * 2), m->wy, },
		{ 1, m->wx,            m->wy, },
		{ 1, m->wx + tw,       m->wy + (2 * th) },
		{ 1, m->wx + (tw * 2), m->wy + (2 * th) },
		{ 1, m->wx,            m->wy + (2 * th) }
	};

	if (ws != c->ws) {
		ws = c->ws;
		index = 0;
	}
	FOR_EACH(t, c->ws->clients)
		if (FLOATING(t) && t != c)
			for (i = 0; i < LEN(q); i++)
				if (q[i][0] && (t->x >= q[i][1] && t->y >= q[i][2]
							&& t->x < q[i][1] + tw && t->y < q[i][2] + th))
				{
					q[i][0] = 0;
					break;
				}
	for (i = 0; i < LEN(q); i++)
		if (q[i][0])
			break;
	if (i == LEN(q)) {
		i = index;
		index = (index + 1) % LEN(q);
	}
	*x = q[i][1] + (((*w - tw) * -1) / 2);
	*y = q[i][2] + (((*h - th) * -1) / 2);
	DBGEXIT("quadrant")
}

int refresh(void)
{
	DBGENTER("refresh")
	Desk *d;
	Panel *p;
	Client *c;
	Monitor *m;
	Workspace *ws;

#define MAP(v, list)                         \
	do {                                     \
		FOR_EACH(v, list)                    \
			if (v->state & STATE_NEEDSMAP) { \
				v->state &= ~STATE_NEEDSMAP; \
				xcb_map_window(con, v->win); \
			}                                \
	} while (0)

	MAP(p, panels);
	MAP(d, desks);
	FOR_EACH(ws, workspaces) {
		showhide(ws->stack);
		if (ws == ws->mon->ws && ws->layout->func)
			ws->layout->func(ws);
		MAP(c, ws->clients);
	}
	focus(NULL);
	for (m = nextmon(monitors); m; m = nextmon(m->next))
		restack(m->ws);
	xcb_aux_sync(con);
	ignore(XCB_ENTER_NOTIFY);
	printstatus(NULL);

	DBGEXIT("refresh")
	return 0;
#undef MAP
}

void relocate(Client *c, Monitor *new, Monitor *old)
{
	DBGENTER("relocate")
#define RELOC(val, opposed, offset, min, max, wmin, wmax, oldmin, oldmax, oldwmin, oldwmax) \
	if (val - oldwmin > 0 && (offset = oldwmax / (val - oldwmin)) != 0.0) {             \
		if (val + (opposed) == oldmin + oldmax) {                                       \
			val = min + max - (opposed);                                                \
		} else if (val + ((opposed) / 2) == oldmin + (oldmax / 2)) {                    \
			val = (min + max - (opposed)) / 2;                                          \
		} else {                                                                        \
			val = CLAMP(min + (max / offset), min - ((opposed) - globalcfg[GLB_MIN_XY]),\
					min + max - globalcfg[GLB_MIN_XY]);                                 \
		}                                                                               \
	} else {                                                                            \
		val = CLAMP(val, min - ((opposed) - globalcfg[GLB_MIN_XY]),                     \
				wmin + wmax - globalcfg[GLB_MIN_XY]);                                   \
	}

	if (!FLOATING(c))
		return;
	DBG("relocate: 0x%08x - current geom: %d,%d %dx%d", c->win, c->x, c->y, c->w, c->h)
	if (c->state & STATE_FULLSCREEN && c->w == old->w && c->h == old->h) {
		c->x = new->x, c->y = new->y, c->w = new->w, c->h = new->h;
	} else {
		float f;
		RELOC(c->x, W(c), f, new->wx, new->ww, new->x, new->w, old->wx, old->ww, old->x, old->w)
		RELOC(c->y, H(c), f, new->wy, new->wh, new->y, new->h, old->wy, old->wh, old->y, old->h)
	}
	DBG("relocate: 0x%08x - new geom: %d,%d %dx%d", c->win, c->x, c->y, c->w, c->h)
	DBGEXIT("relocate")
#undef RELOC
}

void relocatews(Workspace *ws, Monitor *old)
{
	DBGENTER("relocatews")
	Client *c;
	Monitor *new;

	if (!(new = ws->mon) || new == old) return;
	DBG("relocatews: %d:%s -> %d:%s", old->ws->num, old->name, new->ws->num, new->name)
	FOR_EACH(c, ws->clients) relocate(c, new, old);
	DBGEXIT("relocatews")
}

void resize(Client *c, int x, int y, int w, int h, int bw)
{
	DBGENTER("resize")
	c->old_x = c->x, c->old_y = c->y, c->old_w = c->w, c->old_h = c->h;
	c->x = x, c->y = y, c->w = w, c->h = h;
	MOVERESIZE(c->win, x, y, w, h, bw);
	drawborder(c, c == selws->sel);
	sendconfigure(c);
	DBGEXIT("resize")
}

void resizehint(Client *c, int x, int y, int w, int h, int bw, int usermotion, int mouse)
{
	DBGENTER("resizehint")
	if (applysizehints(c, &x, &y, &w, &h, bw, usermotion, mouse))
		resize(c, x, y, w, h, bw);
	DBGEXIT("resizehint")
}

void restack(Workspace *ws)
{
	DBGENTER("restack")
	Desk *d;
	Panel *p;
	Client *c;

	if (!ws || !(c = ws->sel)) return;
	FOR_EACH(p, panels)
		if (p->mon == ws->mon)
			setstackmode(p->win, XCB_STACK_MODE_BELOW);
	if (FLOATING(c))
		setstackmode(c->win, XCB_STACK_MODE_ABOVE);
	if (ws->layout->func && ws == ws->mon->ws)
		for (c = ws->stack; c; c = c->snext)
			if (!(c->state & STATE_FLOATING))
				setstackmode(c->win, XCB_STACK_MODE_BELOW);
	FOR_EACH(d, desks)
		if (d->mon == ws->mon)
			setstackmode(d->win, XCB_STACK_MODE_BELOW);
	DBGEXIT("restack")
}

int rulecmp(Client *c, Rule *r)
{
	DBGENTER("rulecmp")
	DBGEXIT("rulecmp")
	return !((r->class && regexec(&(r->classreg), c->class, 0, NULL, 0))
			|| (r->inst && regexec(&(r->instreg), c->inst, 0, NULL, 0))
			|| (r->title && regexec(&(r->titlereg), c->title, 0, NULL, 0)));
}

void sendconfigure(Client *c)
{
	DBGENTER("sendconfigure")
	xcb_configure_notify_event_t e = {
		.event = c->win,
		.window = c->win,
		.response_type = XCB_CONFIGURE_NOTIFY,
		.x = c->x,
		.y = c->y,
		.width = c->w,
		.height = c->h,
		.border_width = c->bw,
		.above_sibling = XCB_NONE,
		.override_redirect = 0
	};
	xcb_send_event(con, 0, c->win, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (char *)&e);
	DBGEXIT("sendconfigure")
}

int sendwmproto(Client *c, int wmproto)
{
	DBGENTER("sendwmproto")
	int exists = 0;
	xcb_generic_error_t *er;
	xcb_get_property_cookie_t rpc;
	xcb_icccm_get_wm_protocols_reply_t proto;

	rpc = xcb_icccm_get_wm_protocols(con, c->win, wmatom[WM_PROTO]);
	if (xcb_icccm_get_wm_protocols_reply(con, rpc, &proto, &er)) {
		int n = proto.atoms_len;
		while (!exists && n--) exists = proto.atoms[n] == wmatom[wmproto];
		xcb_icccm_get_wm_protocols_reply_wipe(&proto);
	} else {
		iferr(0, "unable to get requested wm protocol", er);
	}
	if (exists) {
		xcb_client_message_event_t e = {
			.response_type = XCB_CLIENT_MESSAGE,
			.window = c->win,
			.type = wmatom[WM_PROTO],
			.format = 32,
			.data.data32[0] = wmatom[wmproto],
			.data.data32[1] = XCB_TIME_CURRENT_TIME
		};
		iferr(0, "unable to send client message event", xcb_request_check(con,
					xcb_send_event_checked(con, 0, c->win, XCB_EVENT_MASK_NO_EVENT, (char *)&e)));
	}
	DBGEXIT("sendwmproto")
	return exists;
}

void setfullscreen(Client *c, int fullscreen)
{
	DBGENTER("setfullscreen")
	Monitor *m;

	if (!c->ws || !(m = c->ws->mon))
		m = selws->mon;
	if (fullscreen && !(c->state & STATE_FULLSCREEN)) {
		PROP(REPLACE, c->win, netatom[NET_WM_STATE], XCB_ATOM_ATOM, 32, 1, &netatom[NET_STATE_FULL]);
		c->old_state = c->state;
		c->state |= STATE_FULLSCREEN | STATE_FLOATING;
		resize(c, m->x, m->y, m->w, m->h, 0);
		setstackmode(c->win, XCB_STACK_MODE_ABOVE);
	} else if (!fullscreen && (c->state & STATE_FULLSCREEN)) {
		PROP(REPLACE, c->win, netatom[NET_WM_STATE], XCB_ATOM_ATOM, 32, 0, (unsigned char *)0);
		c->state = c->old_state;
		resize(c, c->old_x, c->old_y, c->old_w, c->old_h, c->bw);
		needsrefresh = 1;
	}
	DBGEXIT("setfullscreen")
}

void setinputfocus(Client *c)
{
	DBGENTER("setinputfocus")
	if (!(c->state & STATE_NOINPUT)) {
		xcb_set_input_focus(con, XCB_INPUT_FOCUS_POINTER_ROOT, c->win, XCB_CURRENT_TIME);
		PROP(REPLACE, root, netatom[NET_ACTIVE], XCB_ATOM_WINDOW, 32, 1, &c->win);
	}
	sendwmproto(c, WM_FOCUS);
	DBGEXIT("setinputfocus")
}

void setnetwsnames(void)
{
	DBGENTER("setnetwsnames")
	char *names;
	Workspace *ws;
	size_t len = 1;

	FOR_EACH(ws, workspaces)
		len += strlen(ws->name) + 1;
	names = ecalloc(1, len);
	len = 0;
	FOR_EACH(ws, workspaces)
		for (unsigned int i = 0; (names[len++] = ws->name[i]); i++);
	PROP(REPLACE, root, netatom[NET_DESK_NAMES], wmatom[WM_UTF8STR], 8, --len, names);
	free(names);
	DBGEXIT("setnetwsnames")
}

void setstackmode(xcb_window_t win, unsigned int mode)
{
	DBGENTER("setstackmode")
	xcb_configure_window(con, win, XCB_CONFIG_WINDOW_STACK_MODE, &mode);
	DBGEXIT("setstackmode")
}

void seturgent(Client *c, int urg)
{
	DBGENTER("seturgent")
	xcb_generic_error_t *e;
	xcb_icccm_wm_hints_t wmh;
	xcb_get_property_cookie_t pc;

	DBG("seturgent: 0x%08x -> %d", c->win, urg)
	pc = xcb_icccm_get_wm_hints(con, c->win);
	if (urg && c != selws->sel)
		c->state |= STATE_URGENT;
	else if (!urg)
		c->state &= ~STATE_URGENT;
	if (xcb_icccm_get_wm_hints_reply(con, pc, &wmh, &e)) {
		wmh.flags = urg ? (wmh.flags | XCB_ICCCM_WM_HINT_X_URGENCY)
			: (wmh.flags & ~XCB_ICCCM_WM_HINT_X_URGENCY);
		xcb_icccm_set_wm_hints(con, c->win, &wmh);
	} else {
		iferr(0, "unable to get wm window hints", e);
	}
	DBGEXIT("seturgent")
}

void setwinstate(xcb_window_t win, long state)
{
	DBGENTER("setwinstate")
	long data[] = { state, XCB_ATOM_NONE };
	PROP(REPLACE, win, wmatom[WM_STATE], wmatom[WM_STATE], 32, 2, (unsigned char *)data);
	DBGEXIT("setwinstate")
}

void setworkspace(Client *c, int num, int stacktail)
{
	DBGENTER("setworkspace")
	Workspace *ws;
	Client *tail = NULL;

	if (!(ws = itows(num)) || ws == c->ws) return;
	DBG("setworkspace: 0x%08x -> %d", c->win, num)
	if (c->ws) {
		detach(c, 0);
		detachstack(c);
	}
	c->ws = ws;
	PROP(REPLACE, c->win, netatom[NET_WM_DESK], XCB_ATOM_CARDINAL, 32, 1, &c->ws->num);
	attach(c, globalcfg[GLB_TILE_TOHEAD]);
	if (stacktail)
		for (tail = ws->stack; tail && tail->snext; tail = tail->snext)
			;
	if (tail) {
		tail->snext = c;
		c->snext = NULL;
	} else {
		c->snext = c->ws->stack;
		c->ws->stack = c;
	}
	DBGEXIT("setworkspace")
}

void showhide(Client *c)
{
	DBGENTER("showhide")
	Monitor *m;

	if (!c) return;
	m = c->ws->mon;
	if (c->ws == m->ws) {
		if (c->state & STATE_NEEDSRESIZE) {
			MOVERESIZE(c->win, c->x, c->y, c->w, c->h, FULLSCREEN(c) ? 0 : c->bw);
			c->state &= ~STATE_NEEDSRESIZE;
		} else {
			MOVE(c->win, c->x, c->y);
		}
		if (FLOATING(c)) {
			if (c->state & STATE_FULLSCREEN && c->w == m->w && c->h == m->h)
				resize(c, m->x, m->y, m->w, m->h, 0);
			else
				resize(c, c->x, c->y, c->w, c->h, FULLSCREEN(c) ? 0 : c->bw);
		}
		showhide(c->snext);
	} else {
		showhide(c->snext);
		if (!(c->state & STATE_STICKY)) {
			MOVE(c->win, W(c) * -2, c->y);
		} else if (c->ws != selws && m == selws->mon) {
			Client *sel = lastws->sel == c ? c : selws->sel;
			setworkspace(c, selws->num, 0);
			focus(sel);
		}
	}
	DBGEXIT("showhide")
}

void sizehints(Client *c, int uss)
{
	DBGENTER("sizehints")
	xcb_size_hints_t s;
	xcb_generic_error_t *e;
	xcb_get_property_cookie_t pc;

	DBG("sizehints: getting size hints - 0x%08x", c->win)
	pc = xcb_icccm_get_wm_normal_hints(con, c->win);
	c->inc_w = c->inc_h = 0;
	c->max_aspect = c->min_aspect = 0.0;
	c->min_w = c->min_h = c->max_w = c->max_h = c->base_w = c->base_h = 0;
	if (xcb_icccm_get_wm_normal_hints_reply(con, pc, &s, &e)) {
		if (uss && s.flags & XCB_ICCCM_SIZE_HINT_US_SIZE)
			c->w = s.width, c->h = s.height;
		if (uss && s.flags & XCB_ICCCM_SIZE_HINT_US_POSITION)
			c->x = s.x, c->y = s.y;
		if (s.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) {
			c->min_aspect = (float)s.min_aspect_den / s.min_aspect_num;
			c->max_aspect = (float)s.max_aspect_num / s.max_aspect_den;
		}
		if (s.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
			c->max_w = s.max_width, c->max_h = s.max_height;
		if (s.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC)
			c->inc_w = s.width_inc, c->inc_h = s.height_inc;
		if (s.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
			c->base_w = s.base_width, c->base_h = s.base_height;
		else if (s.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
			c->base_w = s.min_width, c->base_h = s.min_height;
		if (s.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
			c->min_w = s.min_width, c->min_h = s.min_height;
		else if (s.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE)
			c->min_w = s.base_width, c->min_h = s.base_height;
	} else {
		iferr(0, "unable to get wm normal hints", e);
	}
	if (c->max_w && c->max_h && c->max_w == c->min_w && c->max_h == c->min_h)
		c->state |= STATE_FIXED | STATE_FLOATING;
	DBGEXIT("sizehints")
}

void unfocus(Client *c, int focusroot)
{
	DBGENTER("unfocus")
	if (c) {
		grabbuttons(c, 0);
		drawborder(c, 0);
	}
	if (focusroot) {
		xcb_set_input_focus(con, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
		xcb_delete_property(con, root, netatom[NET_ACTIVE]);
	}
	DBGEXIT("unfocus")
}

void unmanage(xcb_window_t win, int destroyed)
{
	DBGENTER("unmanage")
	Desk *d;
	Panel *p;
	Client *c;
	void *ptr;
	Workspace *ws;

	if ((ptr = c = wintoclient(win))) {
		if (c->cb && running)
			c->cb->func(c, 1);
		detach(c, 0);
		detachstack(c);
		focus(NULL);
		xcb_aux_sync(con);
		ignore(XCB_ENTER_NOTIFY);
	} else if ((ptr = p = wintopanel(win))) {
		Panel **pp = &panels;
		DETACH(p, pp);
		updstruts(p, 0);
	} else if ((ptr = d = wintodesk(win))) {
		Desk **dd = &desks;
		DETACH(d, dd);
	}

	if (!destroyed) {
		xcb_grab_server(con);
		if (c) {
			xcb_configure_window(con, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH, &c->old_bw);
			xcb_ungrab_button(con, XCB_BUTTON_INDEX_ANY, c->win, XCB_MOD_MASK_ANY);
			if (running) {
				xcb_delete_property(con, c->win, netatom[NET_WM_STATE]);
				xcb_delete_property(con, c->win, netatom[NET_WM_DESK]);
			}
		}
		setwinstate(win, XCB_ICCCM_WM_STATE_WITHDRAWN);
		xcb_aux_sync(con);
		xcb_ungrab_server(con);
	} else {
		xcb_flush(con);
	}

	if (ptr) {
		free(ptr);
		xcb_delete_property(con, root, netatom[NET_CLIENTS]);
		FOR_CLIENTS(c, ws) // NOLINT
			PROP(APPEND, root, netatom[NET_CLIENTS], XCB_ATOM_WINDOW, 32, 1, &c->win);
		FOR_EACH(p, panels)
			PROP(APPEND, root, netatom[NET_CLIENTS], XCB_ATOM_WINDOW, 32, 1, &p->win);
		FOR_EACH(d, desks)
			PROP(APPEND, root, netatom[NET_CLIENTS], XCB_ATOM_WINDOW, 32, 1, &d->win);
		needsrefresh = 1;
	}
	DBGEXIT("unmanage")
}

int updoutputs(xcb_randr_output_t *outs, int nouts, xcb_timestamp_t t)
{
	DBGENTER("updoutputs")
	Monitor *m;
	unsigned int n;
	char name[64];
	int i, nmons, changed = 0;
	xcb_generic_error_t *e;
	xcb_randr_get_crtc_info_cookie_t ck;
	xcb_randr_get_crtc_info_reply_t *crtc;
	xcb_randr_get_output_info_reply_t *o;
	xcb_randr_get_output_info_cookie_t oc[nouts];
	xcb_randr_get_output_primary_reply_t *po = NULL;

	DBG("updoutputs: checking %d outputs for changes", nouts)
	for (i = 0; i < nouts; i++)
		oc[i] = xcb_randr_get_output_info(con, outs[i], t);
	for (i = 0, nmons = 0; i < nouts; i++) {
		if (!(o = xcb_randr_get_output_info_reply(con, oc[i], &e)) || o->crtc == XCB_NONE) {
			iferr(0, "unable to get output info or output has no crtc", e);
		} else if (o->connection == XCB_RANDR_CONNECTION_CONNECTED) {
			ck = xcb_randr_get_crtc_info(con, o->crtc, t);
			crtc = xcb_randr_get_crtc_info_reply(con, ck, &e);
			if (!crtc || !xcb_randr_get_crtc_info_outputs_length(crtc)) {
				iferr(0, "unable to get crtc info reply", e);
				goto next;
			}
			n = xcb_randr_get_output_info_name_length(o) + 1;
			strlcpy(name, (char *)xcb_randr_get_output_info_name(o), MIN(sizeof(name), n));
			FOR_EACH(m, monitors) {
				if (outs[i] != m->id && crtc->x >= m->x && crtc->y >= m->y
						&& crtc->x < m->x + m->w && crtc->y < m->y + m->h)
				{
					DBG("updoutput: %s is a clone of %s", name, m->name)
					goto next;
				}
			}
			if ((m = outputtomon(outs[i]))) {
				if (!m->connected || crtc->x != m->x || crtc->y != m->y
						|| crtc->width != m->w || crtc->height != m->h)
					changed = 1;
				m->num = nmons++;
				m->x = m->wx = crtc->x;
				m->y = m->wy = crtc->y;
				m->w = m->ww = crtc->width;
				m->h = m->wh = crtc->height;
				m->connected = 1;
			} else {
				initmon(nmons++, name, outs[i], crtc->x, crtc->y, crtc->width, crtc->height);
				changed = 1;
			}
			DBG("updoutputs: %s - %d,%d @ %dx%d - changed: %d", name,
					crtc->x, crtc->y, crtc->width, crtc->height, changed)
next:
			free(crtc);
		} else if (o->connection == XCB_RANDR_CONNECTION_DISCONNECTED
				&& (m = outputtomon(outs[i])))
		{
			if (m->connected)
				changed = 1, m->connected = 0, m->num = -1;
		}
		free(o);
	}

	if (changed) {
		po = xcb_randr_get_output_primary_reply(con, xcb_randr_get_output_primary(con, root), NULL);
		if (!po || !(primary = outputtomon(po->output)))
			primary = nextmon(monitors);
		free(po);
	}
	DBGEXIT("updoutputs")
	return changed;
}

int updrandr(void)
{
	DBGENTER("updrandr")
	int changed = 0;
	xcb_generic_error_t *e;
	xcb_randr_get_screen_resources_reply_t *r;
	xcb_randr_get_screen_resources_cookie_t rc;

	rc = xcb_randr_get_screen_resources(con, root);
	if ((r = xcb_randr_get_screen_resources_reply(con, rc, &e))) {
		int n;
		if ((n = xcb_randr_get_screen_resources_outputs_length(r)) <= 0)
			warnx("no monitors available");
		else
			changed = updoutputs(xcb_randr_get_screen_resources_outputs(r), n, r->config_timestamp);
		free(r);
	} else {
		iferr(0, "unable to get screen resources", e);
	}
	DBGEXIT("updrandr")
	return changed;
}

void updstruts(Panel *p, int apply)
{
	DBGENTER("updstruts")
	Panel *n;
	Monitor *m;

	FOR_EACH(m, monitors)
		m->wx = m->x, m->wy = m->y, m->ww = m->w, m->wh = m->h;
	if (p) {
		if (apply && !panels)
			applypanelstrut(p);
		FOR_EACH(n, panels)
			if ((apply || n != p) && (n->l || n->r || n->t || n->b))
				applypanelstrut(p);
	}
	updnetworkspaces();
	DBGEXIT("updstruts")
}

void updnetworkspaces(void)
{
	DBGENTER("updnetworkspaces")
	int v[4];
	Workspace *ws;

	xcb_delete_property(con, root, netatom[NET_DESK_VP]);
	xcb_delete_property(con, root, netatom[NET_DESK_WA]);
	v[0] = scr_w, v[1] = scr_h;
	PROP(REPLACE, root, netatom[NET_DESK_GEOM], XCB_ATOM_CARDINAL, 32, 2, &v);
	PROP(REPLACE, root, netatom[NET_DESK_NUM], XCB_ATOM_CARDINAL, 32, 1, &globalcfg[GLB_WS_NUM]);
	FOR_EACH(ws, workspaces) {
		if (!ws->mon)
			ws->mon = primary;
		v[0] = ws->mon->x, v[1] = ws->mon->y;
		PROP(APPEND, root, netatom[NET_DESK_VP], XCB_ATOM_CARDINAL, 32, 2, &v);
		v[0] = ws->mon->wx, v[1] = ws->mon->wy, v[2] = ws->mon->ww, v[3] = ws->mon->wh;
		PROP(APPEND, root, netatom[NET_DESK_WA], XCB_ATOM_CARDINAL, 32, 4, &v);
	}
	DBGEXIT("updnetworkspaces")
}

void updworkspaces(int needed)
{
	DBGENTER("updworkspaces")
	int n;
	Client *c;
	Monitor *m;
	Workspace *ws;

	for (n = 0, m = nextmon(monitors); m; m = nextmon(m->next), n++)
		;
	if (n < 1 || n > 99 || needed > 99) {
		warnx(n < 1 ? "no connected monitors" : "allocating too many workspaces: max 99");
		return;
	}
	while (n > globalcfg[GLB_WS_NUM] || needed > globalcfg[GLB_WS_NUM]) {
		initws(globalcfg[GLB_WS_NUM]);
		globalcfg[GLB_WS_NUM]++;
	}

	m = nextmon(monitors);
	FOR_EACH(ws, workspaces) {
		m->ws = m->ws ? m->ws : ws;
		ws->mon = m;
		DBG("updworkspaces: %d:%s -> %s - visible: %d", ws->num, ws->name, m->name, ws == m->ws)
		if (!(m = nextmon(m->next)))
			m = nextmon(monitors);
	}

	FOR_CLIENTS(c, ws)
		if (c->state & STATE_FULLSCREEN && ws == ws->mon->ws)
				resize(c, ws->mon->x, ws->mon->y, ws->mon->w, ws->mon->h, c->bw);
	if (!panels) {
		updnetworkspaces();
	} else {
		Panel *p;
		FOR_EACH(p, panels)
			updstruts(p, 1);
	}
	setnetwsnames();
	needsrefresh = 1;
	DBGEXIT("updworkspaces")
}

xcb_get_window_attributes_reply_t *winattr(xcb_window_t win)
{
	DBGENTER("winattr")
	xcb_get_window_attributes_reply_t *wa = NULL;
	xcb_generic_error_t *e;
	xcb_get_window_attributes_cookie_t wc;

	GET(win, wa, wc, e, "attributes", window_attributes);
	DBGEXIT("winattr")
	return wa;
}

xcb_get_geometry_reply_t *wingeom(xcb_window_t win)
{
	DBGENTER("wingeom")
	xcb_get_geometry_reply_t *g = NULL;
	xcb_generic_error_t *e;
	xcb_get_geometry_cookie_t gc;

	GET(win, g, gc, e, "geometry", geometry);
	DBGEXIT("wingeom")
	return g;
}

int winprop(xcb_window_t win, xcb_atom_t prop, xcb_atom_t *ret)
{
	DBGENTER("winprop")
	int i = 0;
	xcb_generic_error_t *e;
	xcb_get_property_cookie_t c;
	xcb_get_property_reply_t *r = NULL;

	c = xcb_get_property(con, 0, win, prop, XCB_ATOM_ANY, 0, 1);
	if ((r = xcb_get_property_reply(con, c, &e)) && xcb_get_property_value_length(r)) {
		i = 1;
		*ret = *(xcb_atom_t *)xcb_get_property_value(r);
	} else {
		iferr(0, "unable to get window property reply", e);
	}
	free(r);
	DBGEXIT("winprop")
	return i;
}

Client *wintoclient(xcb_window_t win)
{
	DBGENTER("wintoclient")
	Client *c;
	Workspace *ws;

	if (win != XCB_WINDOW_NONE && win != root)
		FOR_CLIENTS(c, ws)
			if (c->win == win) {
				DBGEXIT("wintoclient (SUCCESS)")
				return c;
			}
	DBGEXIT("wintoclient (FAILED)")
	return NULL;
}

Panel *wintopanel(xcb_window_t win)
{
	DBGENTER("wintopanel")
	Panel *p;

	if (win != XCB_WINDOW_NONE && win != root)
		FOR_EACH(p, panels)
			if (p->win == win) {
				DBGEXIT("wintopanel (SUCCESS)")
				return p;
			}
	DBGEXIT("wintopanel (FAILED)")
	return NULL;
}

Desk *wintodesk(xcb_window_t win)
{
	DBGENTER("wintodesk")
	Desk *d;

	if (win != XCB_WINDOW_NONE && win != root)
		FOR_EACH(d, desks)
			if (d->win == win) {
				DBGEXIT("wintodesk (SUCCESS)")
				return d;
			}
	DBGEXIT("wintodesk (FAILED)")
	return NULL;
}

xcb_window_t wintrans(xcb_window_t win)
{
	DBGENTER("wintrans")
	xcb_window_t w;
	xcb_generic_error_t *e;

	if (!xcb_icccm_get_wm_transient_for_reply(con,
				xcb_icccm_get_wm_transient_for(con, win), &w, &e))
	{
		iferr(0, "unable to get wm transient for hint", e);
		DBGEXIT("wintrans (FAILED)")
		return XCB_WINDOW_NONE;
	}
	DBGEXIT("wintrans (SUCCESS)")
	return w;
}
