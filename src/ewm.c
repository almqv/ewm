/* See LICENSE file for copyright and license details.
 *
 * ewm is a fork of dwm, the dynamic window manager. Like any other X client
 * it is driven through handling X events. In contrast to other X clients, a
 * window manager selects for SubstructureRedirectMask on the root window, to
 * receive events about window (dis-)appearance. Only one X connection at a
 * time is allowed to select for this event mask.
 *
 * The event handlers are organized in an array which is accessed whenever a
 * new event has been fetched. This allows event dispatching in O(1) time.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag. Clients are organized in a linked client
 * list on each monitor, the focus history is remembered through a stack list
 * on each monitor. Each client contains a bit array to indicate the tags of a
 * client.
 *
 * Settings, keys, buttons and rules come from config.lua (see config.c).
 *
 * To understand everything else, start reading main().
 */
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#include <X11/Xft/Xft.h>

#include "ewm.h"
#include "ipc.h"
#include "util.h"

/* macros */
#define BUTTONMASK (ButtonPressMask | ButtonReleaseMask)
#define CLEANMASK(mask)                                                      \
	(mask & ~(numlockmask | LockMask)                                        \
	 & (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask  \
	    | Mod5Mask))
#define INTERSECT(x, y, w, h, m)                                             \
	(MAX(0, MIN((x) + (w), (m)->mx + (m)->mw) - MAX((x), (m)->mx))           \
	 * MAX(0, MIN((y) + (h), (m)->my + (m)->mh) - MAX((y), (m)->my)))
#define MOUSEMASK (BUTTONMASK | PointerMotionMask)
#define TEXTW(X)  (drw_fontset_getwidth(drw, (X)) + lrpad)

/* enums */
enum { CurNormal, CurResize, CurMove, CurLast }; /* cursor */
enum {
	NetSupported,
	NetWMName,
	NetWMState,
	NetWMCheck,
	NetWMFullscreen,
	NetActiveWindow,
	NetWMWindowType,
	NetWMWindowTypeDialog,
	NetClientList,
	NetLast
}; /* EWMH atoms */
enum {
	WMProtocols,
	WMDelete,
	WMState,
	WMTakeFocus,
	WMLast
}; /* default atoms */

/* function declarations */
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h,
                          int interact);
static void arrangemon(Monitor *m);
static void attach(Client *c);
static void attachtop(Client *c);
static void attachstack(Client *c);
static void buttonpress(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void cleanupmon(Monitor *mon);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(void);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static void detachstack(Client *c);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focusin(XEvent *e);
static Atom getatomprop(Client *c, Atom prop);
static int getrootptr(int *x, int *y);
static long getstate(Window w);
static pid_t getstatusbarpid(void);
static int gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void handlexevents(void);
static void keypress(XEvent *e);
static void manage(Window w, XWindowAttributes *wa);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(Monitor *m);
static void motionnotify(XEvent *e);
static void pop(Client *);
static void propertynotify(XEvent *e);
static Monitor *recttomon(int x, int y, int w, int h);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void run(void);
static void runautostart(void);
static void scan(void);
static int sendevent(Client *c, Atom proto);
static void sendmon(Client *c, Monitor *m);
static void setclientstate(Client *c, long state);
static void setfocus(Client *c);
static void setup(void);
static void setupepoll(void);
static void seturgent(Client *c, int urg);
static void showhide(Client *c);
static void tile(Monitor *);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updateclientlist(void);
static int updategeom(void);
static void updatenumlockmask(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatetitle(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static Monitor *wintomon(Window w);
static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);

/* variables */
static const char broken[] = "broken";
static char stext[256];
static int statusw;
static int statussig;
static pid_t statuspid = -1;
static int screen;
static int sw, sh;      /* X display screen geometry width, height */
static int bh, blw = 0; /* bar geometry */
static int lrpad;       /* sum of left and right padding for text */
static int (*xerrorxlib)(Display *, XErrorEvent *);
static unsigned int numlockmask             = 0;
static void (*handler[LASTEvent])(XEvent *) = {
    [ButtonPress]      = buttonpress,
    [ClientMessage]    = clientmessage,
    [ConfigureRequest] = configurerequest,
    [ConfigureNotify]  = configurenotify,
    [DestroyNotify]    = destroynotify,
    [EnterNotify]      = enternotify,
    [Expose]           = expose,
    [FocusIn]          = focusin,
    [KeyPress]         = keypress,
    [MappingNotify]    = mappingnotify,
    [MapRequest]       = maprequest,
    [MotionNotify]     = motionnotify,
    [PropertyNotify]   = propertynotify,
    [UnmapNotify]      = unmapnotify};
static Atom wmatom[WMLast], netatom[NetLast];
static int dpy_fd;
static int running = 1;
static int reloadpending; /* reload once the current event is handled */
static Cur *cursor[CurLast];
static Clr *scheme[SchemeLast];
static Window wmcheckwin;

Config cfg;
Display *dpy;
Drw *drw;
Monitor *mons, *selmon, *lastselmon;
Window root;
int epoll_fd = -1;

/* built-in layouts; config.c copies them into cfg.layouts */
const Layout builtinlayouts[] = {
    {"tile", "[]=", tile, -1},
    {"floating", "><>", NULL, -1},
    {"monocle", "[M]", monocle, -1},
};
const size_t nbuiltinlayouts = LENGTH(builtinlayouts);

/* commands callable through ewm-msg run_command */
static IPCCommand ipccommands[] = {
    IPCCOMMAND(view, 1, {ARG_TYPE_UINT}),
    IPCCOMMAND(toggleview, 1, {ARG_TYPE_UINT}),
    IPCCOMMAND(tag, 1, {ARG_TYPE_UINT}),
    IPCCOMMAND(toggletag, 1, {ARG_TYPE_UINT}),
    IPCCOMMAND(tagmon, 1, {ARG_TYPE_SINT}),
    IPCCOMMAND(focusmon, 1, {ARG_TYPE_SINT}),
    IPCCOMMAND(focusstack, 1, {ARG_TYPE_SINT}),
    IPCCOMMAND(zoom, 1, {ARG_TYPE_NONE}),
    IPCCOMMAND(incnmaster, 1, {ARG_TYPE_SINT}),
    IPCCOMMAND(killclient, 1, {ARG_TYPE_SINT}),
    IPCCOMMAND(togglefloating, 1, {ARG_TYPE_NONE}),
    IPCCOMMAND(setmfact, 1, {ARG_TYPE_FLOAT}),
    IPCCOMMAND(setlayoutsafe, 1, {ARG_TYPE_PTR}),
    IPCCOMMAND(reload, 1, {ARG_TYPE_NONE}),
    IPCCOMMAND(quit, 1, {ARG_TYPE_NONE})};

/* function implementations */
void applyrules(Client *c) {
	const char *class, *instance;
	unsigned int i;
	const Rule *r;
	Monitor *m;
	XClassHint ch = {NULL, NULL};

	/* rule matching */
	c->isfloating = 0;
	c->tags       = 0;
	XGetClassHint(dpy, c->win, &ch);
	class    = ch.res_class ? ch.res_class : broken;
	instance = ch.res_name ? ch.res_name : broken;

	for (i = 0; i < cfg.nrules; i++) {
		r = &cfg.rules[i];
		if ((!r->title || strstr(c->name, r->title))
		    && (!r->class || strstr(class, r->class))
		    && (!r->instance || strstr(instance, r->instance))) {
			c->isfloating = r->isfloating;
			c->tags |= r->tags;
			for (m = mons; m && m->num != r->monitor; m = m->next)
				;
			if (m)
				c->mon = m;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
	c->tags = c->tags & TAGMASK ? c->tags & TAGMASK
	                            : c->mon->tagset[c->mon->seltags];
}

int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact) {
	int baseismin;
	Monitor *m = c->mon;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw)
			*x = sw - WIDTH(c);
		if (*y > sh)
			*y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0)
			*x = 0;
		if (*y + *h + 2 * c->bw < 0)
			*y = 0;
	} else {
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (cfg.resizehints || c->isfloating
	    || !c->mon->lt[c->mon->sellt]->arrange) {
		/* see last two sentences in ICCCM 4.1.2.3 */
		baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) { /* temporarily remove base dimensions */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for aspect limits */
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float) *w / *h)
				*w = *h * c->maxa + 0.5;
			else if (c->mina < (float) *h / *w)
				*h = *w * c->mina + 0.5;
		}
		if (baseismin) { /* increment calculation requires this */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for increment value */
		if (c->incw)
			*w -= *w % c->incw;
		if (c->inch)
			*h -= *h % c->inch;
		/* restore base dimensions */
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw)
			*w = MIN(*w, c->maxw);
		if (c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void arrange(Monitor *m) {
	if (m)
		showhide(m->stack);
	else
		for (m = mons; m; m = m->next)
			showhide(m->stack);
	if (m) {
		arrangemon(m);
		restack(m);
	} else
		for (m = mons; m; m = m->next)
			arrangemon(m);
}

void arrangemon(Monitor *m) {
	snprintf(m->ltsymbol, sizeof m->ltsymbol, "%s", m->lt[m->sellt]->symbol);
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

void attach(Client *c) {
	c->next         = c->mon->clients;
	c->mon->clients = c;
}

void attachtop(Client *c) {
	int n;
	Monitor *m = c->mon;
	Client *below;

	for (n = 1, below = c->mon->clients;
	     below && below->next
	     && (below->isfloating || !ISVISIBLEONTAG(below, c->tags)
	         || n != m->nmaster);
	     n    = below->isfloating || !ISVISIBLEONTAG(below, c->tags) ? n + 0
	                                                                 : n + 1,
	    below = below->next)
		;
	c->next = NULL;
	if (below) {
		c->next     = below->next;
		below->next = c;
	} else
		c->mon->clients = c;
}

void attachstack(Client *c) {
	c->snext      = c->mon->stack;
	c->mon->stack = c;
}

void buttonpress(XEvent *e) {
	unsigned int i, x, click;
	int tagnum = 0;
	Arg arg = {0};
	Client *c;
	Monitor *m;
	XButtonPressedEvent *ev = &e->xbutton;
	char *text, *s, ch;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(ev->window)) && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if (ev->window == selmon->barwin) {
		i = x = 0;
		do
			x += TEXTW(cfg.tags[i]);
		while (ev->x >= x && ++i < cfg.ntags);
		if (i < cfg.ntags) {
			click  = ClkTagBar;
			arg.ui = 1 << i;
			tagnum = i + 1;
		} else if (ev->x < x + blw)
			click = ClkLtSymbol;
		else if (ev->x > selmon->ww - statusw) {
			x         = selmon->ww - statusw;
			click     = ClkStatusText;
			statussig = 0;
			for (text = s = stext; *s && x <= ev->x; s++) {
				if ((unsigned char) (*s) < ' ') {
					ch = *s;
					*s = '\0';
					x += TEXTW(text) - lrpad;
					*s   = ch;
					text = s + 1;
					if (x >= ev->x)
						break;
					statussig = ch;
				}
			}
		} else
			click = ClkWinTitle;
	} else if ((c = wintoclient(ev->window))) {
		focus(c);
		restack(selmon);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		click = ClkClientWin;
	}
	for (i = 0; i < cfg.nbuttons; i++) {
		/* a Lua callback may rebind buttons, so work on a copy */
		Button b = cfg.buttons[i];

		if (click != b.click || !b.func || b.button != ev->button
		    || CLEANMASK(b.mask) != CLEANMASK(ev->state))
			continue;
		if (b.func == config_call)
			config_callbutton(&b.arg, tagnum);
		else
			b.func(click == ClkTagBar && b.arg.i == 0 ? &arg : &b.arg);
	}
}

void checkotherwm(void) {
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

void cleanup(void) {
	Arg a      = {.ui = ~0};
	Layout foo = {"", "", NULL, -1};
	Monitor *m;
	size_t i;

	view(&a);
	selmon->lt[selmon->sellt] = &foo;
	for (m = mons; m; m = m->next)
		while (m->stack)
			unmanage(m->stack, 0);
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	while (mons)
		cleanupmon(mons);
	for (i = 0; i < CurLast; i++)
		drw_cur_free(drw, cursor[i]);
	for (i = 0; i < SchemeLast; i++)
		free(scheme[i]);
	XDestroyWindow(dpy, wmcheckwin);
	drw_free(drw);
	XSync(dpy, False);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);

	ipc_cleanup();

	if (close(epoll_fd) < 0) {
		fprintf(stderr, "Failed to close epoll file descriptor\n");
	}
}

void cleanupmon(Monitor *mon) {
	Monitor *m;

	if (mon == mons)
		mons = mons->next;
	else {
		for (m = mons; m && m->next != mon; m = m->next)
			;
		m->next = mon->next;
	}
	XUnmapWindow(dpy, mon->barwin);
	XDestroyWindow(dpy, mon->barwin);
	if (lastselmon == mon)
		lastselmon = NULL;
	free(mon);
}

void clientmessage(XEvent *e) {
	XClientMessageEvent *cme = &e->xclient;
	Client *c                = wintoclient(cme->window);

	if (!c)
		return;
	if (cme->message_type == netatom[NetWMState]) {
		if (cme->data.l[1] == netatom[NetWMFullscreen]
		    || cme->data.l[2] == netatom[NetWMFullscreen])
			setfullscreen(c,
			              (cme->data.l[0] == 1     /* _NET_WM_STATE_ADD    */
			               || (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */
			                   && !c->isfullscreen)));
	} else if (cme->message_type == netatom[NetActiveWindow]) {
		if (c != selmon->sel && !c->isurgent)
			seturgent(c, 1);
	}
}

void configure(Client *c) {
	XConfigureEvent ce;

	ce.type              = ConfigureNotify;
	ce.display           = dpy;
	ce.event             = c->win;
	ce.window            = c->win;
	ce.x                 = c->x;
	ce.y                 = c->y;
	ce.width             = c->w;
	ce.height            = c->h;
	ce.border_width      = c->bw;
	ce.above             = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *) &ce);
}

void configurenotify(XEvent *e) {
	Monitor *m;
	Client *c;
	XConfigureEvent *ev = &e->xconfigure;
	int dirty;

	/* TODO: updategeom handling sucks, needs to be simplified */
	if (ev->window == root) {
		dirty = (sw != ev->width || sh != ev->height);
		sw    = ev->width;
		sh    = ev->height;
		if (updategeom() || dirty) {
			drw_resize(drw, sw, bh);
			updatebars();
			for (m = mons; m; m = m->next) {
				for (c = m->clients; c; c = c->next)
					if (c->isfullscreen)
						resizeclient(c, m->mx, m->my, m->mw, m->mh);
				XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, m->ww, bh);
			}
			focus(NULL);
			arrange(NULL);
		}
	}
}

void configurerequest(XEvent *e) {
	Client *c;
	Monitor *m;
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & CWBorderWidth)
			c->bw = ev->border_width;
		else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
			m = c->mon;
			if (ev->value_mask & CWX) {
				c->oldx = c->x;
				c->x    = m->mx + ev->x;
			}
			if (ev->value_mask & CWY) {
				c->oldy = c->y;
				c->y    = m->my + ev->y;
			}
			if (ev->value_mask & CWWidth) {
				c->oldw = c->w;
				c->w    = ev->width;
			}
			if (ev->value_mask & CWHeight) {
				c->oldh = c->h;
				c->h    = ev->height;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx
				     + (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y =
				    m->my
				    + (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((ev->value_mask & (CWX | CWY))
			    && !(ev->value_mask & (CWWidth | CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		} else
			configure(c);
	} else {
		wc.x            = ev->x;
		wc.y            = ev->y;
		wc.width        = ev->width;
		wc.height       = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling      = ev->above;
		wc.stack_mode   = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

/* index of px in cfg.gapmodes, so switchgaps continues from there */
static int gapindex(unsigned int px) {
	size_t i;

	for (i = 0; i < cfg.ngapmodes; i++)
		if (cfg.gapmodes[i] == px)
			return i;
	return 0;
}

Monitor *createmon(void) {
	Monitor *m;

	m            = ecalloc(1, sizeof(Monitor));
	m->tagset[0] = m->tagset[1] = 1;
	m->mfact                    = cfg.mfact;
	m->nmaster                  = cfg.nmaster;
	m->showbar                  = cfg.showbar;
	m->topbar                   = cfg.topbar;
	m->gappx                    = cfg.gappx;
	m->gapidx                   = gapindex(cfg.gappx);
	m->lt[0]                    = &cfg.layouts[0];
	m->lt[1]                    = &cfg.layouts[1 % cfg.nlayouts];
	snprintf(m->ltsymbol, sizeof m->ltsymbol, "%s", cfg.layouts[0].symbol);
	return m;
}

void destroynotify(XEvent *e) {
	Client *c;
	XDestroyWindowEvent *ev = &e->xdestroywindow;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
}

void detach(Client *c) {
	Client **tc;

	for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next)
		;
	*tc = c->next;
}

void detachstack(Client *c) {
	Client **tc, *t;

	for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext)
		;
	*tc = c->snext;

	if (c == c->mon->sel) {
		for (t = c->mon->stack; t && !ISVISIBLE(t); t = t->snext)
			;
		c->mon->sel = t;
	}
}

Monitor *dirtomon(int dir) {
	Monitor *m = NULL;

	if (dir > 0) {
		if (!(m = selmon->next))
			m = mons;
	} else if (selmon == mons)
		for (m = mons; m->next; m = m->next)
			;
	else
		for (m = mons; m->next != selmon; m = m->next)
			;
	return m;
}

void drawbar(Monitor *m) {
	if (!m->showbar)
		return;

	int x, w, tw = 0;
	int boxs = drw->fonts->h / 9;
	int boxw = drw->fonts->h / 6 + 2;
	unsigned int i, occ = 0, urg = 0;
	Client *c;

	/* draw status first so it can be overdrawn by tags later */
	if (m == selmon) { /* status is only drawn on selected monitor */
		char *text, *s, ch;
		drw_setscheme(drw, scheme[SchemeNorm]);

		x = 0;
		for (text = s = stext; *s; s++) {
			if ((unsigned char) (*s) < ' ') {
				ch = *s;
				*s = '\0';
				tw = TEXTW(text) - lrpad;
				drw_text(drw, m->ww - statusw + x, 0, tw, bh, 0, text, 0);
				x += tw;
				*s   = ch;
				text = s + 1;
			}
		}
		tw = TEXTW(text) - lrpad + 2;
		drw_text(drw, m->ww - statusw + x, 0, tw, bh, 0, text, 0);
		tw = statusw;
	}

	for (c = m->clients; c; c = c->next) {
		occ |= c->tags;
		if (c->isurgent)
			urg |= c->tags;
	}
	x = 0;
	for (i = 0; i < cfg.ntags; i++) {
		w = TEXTW(cfg.tags[i]);
		drw_setscheme(
		    drw,
		    scheme[m->tagset[m->seltags] & 1 << i ? SchemeSel : SchemeNorm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, cfg.tags[i], urg & 1 << i);
		if (occ & 1 << i)
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
			         m == selmon && selmon->sel && selmon->sel->tags & 1 << i,
			         urg & 1 << i);
		x += w;
	}
	w = blw = TEXTW(m->ltsymbol);
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, w, bh, lrpad / 2, m->ltsymbol, 0);

	if ((w = m->ww - tw - x) > bh) {
		if (m->sel) {
			drw_setscheme(drw, scheme[m == selmon ? SchemeSel : SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, m->sel->name, 0);
			if (m->sel->isfloating)
				drw_rect(drw, x + boxs, boxs, boxw, boxw, m->sel->isfixed, 0);
		} else {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x, 0, w, bh, 1, 1);
		}
	}
	drw_map(drw, m->barwin, 0, 0, m->ww, bh);
}

void drawbars(void) {
	Monitor *m;

	for (m = mons; m; m = m->next)
		drawbar(m);
}

void enternotify(XEvent *e) {
	Client *c;
	Monitor *m;
	XCrossingEvent *ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior)
	    && ev->window != root)
		return;
	c = wintoclient(ev->window);
	m = c ? c->mon : wintomon(ev->window);
	if (m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
	} else if (!c || c == selmon->sel)
		return;
	focus(c);
}

void expose(XEvent *e) {
	Monitor *m;
	XExposeEvent *ev = &e->xexpose;

	if (ev->count == 0 && (m = wintomon(ev->window)))
		drawbar(m);
}

static Client *hookfocused; /* client last reported to the focus hook */

void focus(Client *c) {
	if (!c || !ISVISIBLE(c))
		for (c = selmon->stack; c && !ISVISIBLE(c); c = c->snext)
			;
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (c->mon != selmon)
			selmon = c->mon;
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);
		setfocus(c);
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	selmon->sel = c;
	drawbars();
	if (c != hookfocused) {
		hookfocused = c;
		config_hook("focus", c);
	}
}

/* there are some broken focus acquiring clients needing extra handling */
void focusin(XEvent *e) {
	XFocusChangeEvent *ev = &e->xfocus;

	if (selmon->sel && ev->window != selmon->sel->win)
		setfocus(selmon->sel);
}

void focusmon(const Arg *arg) {
	Monitor *m;

	if (!mons->next)
		return;
	if ((m = dirtomon(arg->i)) == selmon)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	focus(NULL);
}

void focusstack(const Arg *arg) {
	Client *c = NULL, *i;

	if (!selmon->sel)
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next)
			;
		if (!c)
			for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next)
				;
	} else {
		for (i = selmon->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c) {
		focus(c);
		restack(selmon);
	}
}

Atom getatomprop(Client *c, Atom prop) {
	int format;
	unsigned long nitems, after;
	unsigned char *p = NULL;
	Atom type, atom = None;

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, XA_ATOM,
	                       &type, &format, &nitems, &after, &p)
	        == Success
	    && p) {
		if (nitems > 0 && format == 32)
			atom = *(long *) p;
		XFree(p);
	}
	return atom;
}

/* whether the basename of argv[0] of process pid is name */
static int cmdlineis(pid_t pid, const char *name) {
	char path[64], buf[256], *base;
	size_t n;
	FILE *fp;

	snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long) pid);
	if (!(fp = fopen(path, "r")))
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	buf[n] = '\0'; /* argv[0] ends at the first NUL */
	base   = strrchr(buf, '/');
	return n > 0 && !strcmp(base ? base + 1 : buf, name);
}

pid_t getstatusbarpid(void) {
	DIR *dir;
	struct dirent *e;
	char *end;
	long pid;

	if (!cfg.statusbar || !*cfg.statusbar)
		return -1;
	if (statuspid > 0 && cmdlineis(statuspid, cfg.statusbar))
		return statuspid;
	if (!(dir = opendir("/proc")))
		return -1;
	while ((e = readdir(dir))) {
		pid = strtol(e->d_name, &end, 10);
		if (!*end && pid > 0 && cmdlineis(pid, cfg.statusbar)) {
			closedir(dir);
			return pid;
		}
	}
	closedir(dir);
	return -1;
}

int getrootptr(int *x, int *y) {
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long getstate(Window w) {
	int format;
	long result      = -1;
	unsigned char *p = NULL;
	unsigned long n, extra;
	Atom real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False,
	                       wmatom[WMState], &real, &format, &n, &extra,
	                       (unsigned char **) &p)
	    != Success)
		return -1;
	if (n != 0 && format == 32)
		result = *(long *) p;
	XFree(p);
	return result;
}

int gettextprop(Window w, Atom atom, char *text, unsigned int size) {
	char **list = NULL;
	int n;
	XTextProperty name;

	if (!text || size == 0)
		return 0;
	text[0] = '\0';
	if (!XGetTextProperty(dpy, w, &name, atom) || !name.nitems)
		return 0;
	if (name.encoding == XA_STRING)
		strncpy(text, (char *) name.value, size - 1);
	else {
		if (XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success
		    && n > 0 && *list) {
			strncpy(text, *list, size - 1);
			XFreeStringList(list);
		}
	}
	text[size - 1] = '\0';
	XFree(name.value);
	return 1;
}

void grabbuttons(Client *c, int focused) {
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = {0, LockMask, numlockmask,
		                            numlockmask | LockMask};
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		if (!focused)
			XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
			            BUTTONMASK, GrabModeSync, GrabModeSync, None, None);
		for (i = 0; i < cfg.nbuttons; i++)
			if (cfg.buttons[i].click == ClkClientWin)
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabButton(dpy, cfg.buttons[i].button,
					            cfg.buttons[i].mask | modifiers[j], c->win,
					            False, BUTTONMASK, GrabModeAsync,
					            GrabModeSync, None, None);
	}
}

void grabkeys(void) {
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = {0, LockMask, numlockmask,
		                            numlockmask | LockMask};
		int k, start, end, skip;
		KeySym *syms;

		XUngrabKey(dpy, AnyKey, AnyModifier, root);
		XDisplayKeycodes(dpy, &start, &end);
		syms = XGetKeyboardMapping(dpy, start, end - start + 1, &skip);
		if (!syms)
			return;
		/* grab every keycode producing the keysym, not just the first one */
		for (k = start; k <= end; k++)
			for (i = 0; i < cfg.nkeys; i++)
				if (cfg.keys[i].keysym == syms[(k - start) * skip])
					for (j = 0; j < LENGTH(modifiers); j++)
						XGrabKey(dpy, k, cfg.keys[i].mod | modifiers[j], root,
						         True, GrabModeAsync, GrabModeAsync);
		XFree(syms);
	}
}

/* handle every X event, both those still on the socket and those Xlib
 * already moved into its queue (e.g. during an XSync in an IPC command);
 * XPending also flushes the output buffer before epoll_wait blocks */
void handlexevents(void) {
	XEvent ev;

	while (running && XPending(dpy)) {
		XNextEvent(dpy, &ev);
		if (handler[ev.type]) {
			handler[ev.type](&ev); /* call handler */
			ipc_send_events();
		}
	}
}

void incnmaster(const Arg *arg) {
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

#ifdef XINERAMA
static int isuniquegeom(XineramaScreenInfo *unique, size_t n,
                        XineramaScreenInfo *info) {
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
		    && unique[n].width == info->width
		    && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

void keypress(XEvent *e) {
	unsigned int i;
	KeySym keysym;
	XKeyEvent *ev;

	ev     = &e->xkey;
	keysym = XkbKeycodeToKeysym(dpy, (KeyCode) ev->keycode, 0, 0);
	for (i = 0; i < cfg.nkeys; i++) {
		/* a Lua callback may rebind keys, so work on a copy */
		Key k = cfg.keys[i];

		if (keysym == k.keysym && CLEANMASK(k.mod) == CLEANMASK(ev->state)
		    && k.func)
			k.func(&k.arg);
	}
}

void closeclient(Client *c) {
	if (!sendevent(c, wmatom[WMDelete])) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, c->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
}

void killclient(const Arg *arg) {
	if (selmon->sel)
		closeclient(selmon->sel);
}

void manage(Window w, XWindowAttributes *wa) {
	Client *c, *t = NULL;
	Window trans = None;
	XWindowChanges wc;

	c      = ecalloc(1, sizeof(Client));
	c->win = w;
	/* geometry */
	c->x = c->oldx = wa->x;
	c->y = c->oldy = wa->y;
	c->w = c->oldw = wa->width;
	c->h = c->oldh = wa->height;
	c->oldbw       = wa->border_width;

	updatetitle(c);
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->mon  = t->mon;
		c->tags = t->tags;
	} else {
		c->mon = selmon;
		applyrules(c);
	}

	c->bw = cfg.borderpx;
	if (c->x + WIDTH(c) > c->mon->wx + c->mon->ww)
		c->x = c->mon->wx + c->mon->ww - WIDTH(c);
	if (c->y + HEIGHT(c) > c->mon->wy + c->mon->wh)
		c->y = c->mon->wy + c->mon->wh - HEIGHT(c);
	c->x = MAX(c->x, c->mon->wx);
	c->y = MAX(c->y, c->mon->wy);
	/* windows without a position preference end up at the origin of the
	 * window area; center them instead */
	if (c->x == c->mon->wx)
		c->x = MAX(c->mon->wx, c->mon->wx + (c->mon->ww - WIDTH(c)) / 2);
	if (c->y == c->mon->wy)
		c->y = MAX(c->mon->wy, c->mon->wy + (c->mon->wh - HEIGHT(c)) / 2);

	wc.border_width = c->bw;

	XConfigureWindow(dpy, w, CWBorderWidth, &wc);
	XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);
	configure(c); /* propagates border_width, if size doesn't change */
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);
	XSelectInput(dpy, w,
	             EnterWindowMask | FocusChangeMask | PropertyChangeMask
	                 | StructureNotifyMask);
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = trans != None || c->isfixed;
	if (c->isfloating)
		XRaiseWindow(dpy, c->win);
	attachtop(c);
	attachstack(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
	                PropModeAppend, (unsigned char *) &(c->win), 1);
	XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w,
	                  c->h); /* some windows require this */
	setclientstate(c, NormalState);
	if (c->mon == selmon)
		unfocus(selmon->sel, 0);
	c->mon->sel = c;
	arrange(c->mon);
	XMapWindow(dpy, c->win);
	focus(NULL);
	config_hook("manage", c);
}

void mappingnotify(XEvent *e) {
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabkeys();
}

void maprequest(XEvent *e) {
	static XWindowAttributes wa;
	XMapRequestEvent *ev = &e->xmaprequest;

	if (!XGetWindowAttributes(dpy, ev->window, &wa))
		return;
	if (wa.override_redirect)
		return;
	if (!wintoclient(ev->window))
		manage(ev->window, &wa);
}

void monocle(Monitor *m) {
	unsigned int n = 0;
	Client *c;

	for (c = m->clients; c; c = c->next)
		if (ISVISIBLE(c))
			n++;
	if (n > 0) /* override layout symbol */
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		resize(c, m->wx, m->wy, m->ww - 2 * c->bw, m->wh - 2 * c->bw, 0);
}

void motionnotify(XEvent *e) {
	static Monitor *mon = NULL;
	Monitor *m;
	XMotionEvent *ev = &e->xmotion;

	if (ev->window != root)
		return;
	if ((m = recttomon(ev->x_root, ev->y_root, 1, 1)) != mon && mon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	mon = m;
}

void movemouse(const Arg *arg) {
	int x, y, ocx, ocy, nx, ny;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen) /* no support moving fullscreen windows by mouse */
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync,
	                 GrabModeAsync, None, cursor[CurMove]->cursor,
	                 CurrentTime)
	    != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	do {
		XMaskEvent(dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask,
		           &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

			nx = ocx + (ev.xmotion.x - x);
			ny = ocy + (ev.xmotion.y - y);
			if (abs(selmon->wx - nx) < cfg.snap)
				nx = selmon->wx;
			else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < cfg.snap)
				nx = selmon->wx + selmon->ww - WIDTH(c);
			if (abs(selmon->wy - ny) < cfg.snap)
				ny = selmon->wy;
			else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < cfg.snap)
				ny = selmon->wy + selmon->wh - HEIGHT(c);
			if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
			    && (abs(nx - c->x) > cfg.snap || abs(ny - c->y) > cfg.snap))
				togglefloating(NULL);
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, c->w, c->h, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

/* the selected client if it may be moved/resized from the keyboard */
static Client *movableclient(void) {
	Client *c = selmon->sel;

	if (!c || c->isfullscreen
	    || (selmon->lt[selmon->sellt]->arrange && !c->isfloating))
		return NULL;
	return c;
}

/* raise and resize c, dragging the pointer along if it was inside c so that
 * sloppy focus does not move to another window */
static void moveresizeclient(Client *c, int nx, int ny, int nw, int nh) {
	int ox = c->x, oy = c->y, ow = c->w, oh = c->h;
	int msx, msy, di;
	unsigned int dui;
	Window dummy;
	Bool xqp;

	XRaiseWindow(dpy, c->win);
	xqp = XQueryPointer(dpy, root, &dummy, &dummy, &msx, &msy, &di, &di, &dui);
	resize(c, nx, ny, nw, nh, True);
	if (xqp && ox <= msx && ox + ow >= msx && oy <= msy && oy + oh >= msy)
		XWarpPointer(dpy, None, None, 0, 0, 0, 0, c->x - ox + c->w - ow,
		             c->y - oy + c->h - oh);
}

void moveresize(const Arg *arg) {
	Client *c;
	int x, y, w, h, nx, ny, nw, nh;
	char xAbs, yAbs, wAbs, hAbs;

	if (!arg || !arg->v || !(c = movableclient()))
		return;
	if (sscanf((char *) arg->v, "%d%c %d%c %d%c %d%c", &x, &xAbs, &y, &yAbs,
	           &w, &wAbs, &h, &hAbs)
	    != 8)
		return;

	/* compute new window position; prevent window from be positioned outside
	 * the current monitor */
	nw = c->w + w;
	if (wAbs == 'W')
		nw = w < selmon->mw - 2 * c->bw ? w : selmon->mw - 2 * c->bw;

	nh = c->h + h;
	if (hAbs == 'H')
		nh = h < selmon->mh - 2 * c->bw ? h : selmon->mh - 2 * c->bw;

	nx = c->x + x;
	if (xAbs == 'X') {
		if (x < selmon->mx)
			nx = selmon->mx;
		else if (x > selmon->mx + selmon->mw)
			nx = selmon->mx + selmon->mw - nw - 2 * c->bw;
		else
			nx = x;
	}

	ny = c->y + y;
	if (yAbs == 'Y') {
		if (y < selmon->my)
			ny = selmon->my;
		else if (y > selmon->my + selmon->mh)
			ny = selmon->my + selmon->mh - nh - 2 * c->bw;
		else
			ny = y;
	}

	moveresizeclient(c, nx, ny, nw, nh);
}

/* move (t, b, l, r) or grow (T, B, L, R) a floating window to an edge of the
 * window area; growing an already grown window restores its previous size */
void moveresizeedge(const Arg *arg) {
	Client *c;
	char e;
	int nx, ny, nw, nh;
	int wx = selmon->wx, wy = selmon->wy;
	int wr = selmon->wx + selmon->ww, wb = selmon->wy + selmon->wh;

	if (!arg || !arg->v || !(c = movableclient()))
		return;
	if (sscanf((char *) arg->v, "%c", &e) != 1)
		return;

	nx = c->x;
	ny = c->y;
	nw = c->w;
	nh = c->h;
	switch (e) {
	case 't':
		ny = wy;
		break;
	case 'b':
		ny = wb - HEIGHT(c);
		break;
	case 'l':
		nx = wx;
		break;
	case 'r':
		nx = wr - WIDTH(c);
		break;
	case 'T':
		if (c->y == wy && c->oldy + c->oldh == c->y + c->h) {
			ny = c->oldy;
			nh = c->oldh;
		} else {
			ny = wy;
			nh = c->y + c->h - wy;
		}
		break;
	case 'B':
		if (c->y + HEIGHT(c) == wb && c->oldy == c->y)
			nh = c->oldh;
		else
			nh = wb - c->y - 2 * c->bw;
		break;
	case 'L':
		if (c->x == wx && c->oldx + c->oldw == c->x + c->w) {
			nx = c->oldx;
			nw = c->oldw;
		} else {
			nx = wx;
			nw = c->x + c->w - wx;
		}
		break;
	case 'R':
		if (c->x + WIDTH(c) == wr && c->oldx == c->x)
			nw = c->oldw;
		else
			nw = wr - c->x - 2 * c->bw;
		break;
	default:
		return;
	}
	moveresizeclient(c, nx, ny, nw, nh);
}

Client *nexttiled(Client *c) {
	for (; c && (c->isfloating || !ISVISIBLE(c)); c = c->next)
		;
	return c;
}

void pop(Client *c) {
	detach(c);
	attach(c);
	focus(c);
	arrange(c->mon);
}

void propertynotify(XEvent *e) {
	Client *c;
	Window trans;
	XPropertyEvent *ev = &e->xproperty;

	if ((ev->window == root) && (ev->atom == XA_WM_NAME))
		updatestatus();
	else if (ev->state == PropertyDelete)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch (ev->atom) {
		default:
			break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating && (XGetTransientForHint(dpy, c->win, &trans))
			    && (c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case XA_WM_NORMAL_HINTS:
			updatesizehints(c);
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			drawbars();
			break;
		}
		if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				drawbar(c->mon);
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
	}
}

void quit(const Arg *arg) {
	running = 0;
}

void reload(const Arg *arg) {
	reloadpending = 1;
}

Monitor *recttomon(int x, int y, int w, int h) {
	Monitor *m, *r = selmon;
	int a, area    = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r    = m;
		}
	return r;
}

void resize(Client *c, int x, int y, int w, int h, int interact) {
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void resizeclient(Client *c, int x, int y, int w, int h) {
	XWindowChanges wc;

	c->oldx = c->x;
	c->x = wc.x = x;
	c->oldy     = c->y;
	c->y = wc.y = y;
	c->oldw     = c->w;
	c->w = wc.width = w;
	c->oldh         = c->h;
	c->h = wc.height = h;
	wc.border_width  = c->bw;
	XConfigureWindow(dpy, c->win,
	                 CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &wc);
	configure(c);
	XSync(dpy, False);
}

void resizemouse(const Arg *arg) {
	int ocx, ocy, nw, nh;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen) /* no support resizing fullscreen windows by mouse */
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync,
	                 GrabModeAsync, None, cursor[CurResize]->cursor,
	                 CurrentTime)
	    != GrabSuccess)
		return;
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1,
	             c->h + c->bw - 1);
	do {
		XMaskEvent(dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask,
		           &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

			nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
			nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);
			if (c->mon->wx + nw >= selmon->wx
			    && c->mon->wx + nw <= selmon->wx + selmon->ww
			    && c->mon->wy + nh >= selmon->wy
			    && c->mon->wy + nh <= selmon->wy + selmon->wh) {
				if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
				    && (abs(nw - c->w) > cfg.snap || abs(nh - c->h) > cfg.snap))
					togglefloating(NULL);
			}
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, c->x, c->y, nw, nh, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1,
	             c->h + c->bw - 1);
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
		;
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

void restack(Monitor *m) {
	Client *c;
	XEvent ev;
	XWindowChanges wc;

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || !m->lt[m->sellt]->arrange)
		XRaiseWindow(dpy, m->sel->win);
	if (m->lt[m->sellt]->arrange) {
		wc.stack_mode = Below;
		wc.sibling    = m->barwin;
		for (c = m->stack; c; c = c->snext)
			if (!c->isfloating && ISVISIBLE(c)) {
				XConfigureWindow(dpy, c->win, CWSibling | CWStackMode, &wc);
				wc.sibling = c->win;
			}
	}
	XSync(dpy, False);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
		;
}

void run(void) {
	enum { MAX_EVENTS = 10 };
	struct epoll_event events[MAX_EVENTS];
	int i, n, fd;

	XSync(dpy, False);

	/* main event loop */
	while (running) {
		handlexevents();
		if (reloadpending) {
			reloadpending = 0;
			config_reload();
			continue;
		}
		if (!running)
			break;
		if ((n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1)) < 0) {
			if (errno == EINTR)
				continue;
			die("epoll_wait:");
		}
		for (i = 0; i < n; i++) {
			fd = events[i].data.fd;
			DEBUG("Got event from fd %d\n", fd);

			if (fd == dpy_fd) {
				if (!(events[i].events & EPOLLIN)
				    && events[i].events & (EPOLLHUP | EPOLLERR))
					return; /* X server went away */
				/* events are read by handlexevents() at the loop top */
			} else if (fd == ipc_get_sock_fd()) {
				ipc_handle_socket_epoll_event(events + i);
			} else if (ipc_is_client_registered(fd)) {
				if (ipc_handle_client_epoll_event(events + i) < 0)
					fprintf(stderr, "ewm: error handling IPC event on fd %d\n",
					        fd);
			} else if (!config_handlefd(fd)) {
				fprintf(stderr, "ewm: event from unknown fd %d\n", fd);
			}
		}
		/* timers and IPC commands change state outside X event handlers */
		ipc_send_events();
	}
}

/* common setup of forked children before exec */
static void childsetup(void) {
	struct sigaction sa;

	if (dpy)
		close(ConnectionNumber(dpy));
	setsid();
	sigemptyset(&sa.sa_mask);
	sa.sa_flags   = 0;
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
}

/* start $XDG_CONFIG_HOME/ewm/autostart.sh (~/.config/ewm) in the background */
void runautostart(void) {
	char path[PATH_MAX];

	if (configpath(path, sizeof(path), "autostart.sh") < 0
	    || access(path, X_OK) != 0)
		return;
	if (fork() == 0) {
		childsetup();
		execl(path, path, (char *) NULL);
		fprintf(stderr, "ewm: execl %s", path);
		perror(" failed");
		exit(EXIT_FAILURE);
	}
}

void scan(void) {
	unsigned int i, num;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
		for (i = 0; i < num; i++) {
			if (!XGetWindowAttributes(dpy, wins[i], &wa)
			    || wa.override_redirect
			    || XGetTransientForHint(dpy, wins[i], &d1))
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
		for (i = 0; i < num; i++) { /* now the transients */
			if (!XGetWindowAttributes(dpy, wins[i], &wa))
				continue;
			if (XGetTransientForHint(dpy, wins[i], &d1)
			    && (wa.map_state == IsViewable
			        || getstate(wins[i]) == IconicState))
				manage(wins[i], &wa);
		}
		if (wins)
			XFree(wins);
	}
}

void sendmon(Client *c, Monitor *m) {
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detach(c);
	detachstack(c);
	c->mon  = m;
	c->tags = m->tagset[m->seltags]; /* assign tags of target monitor */
	attachtop(c);
	attachstack(c);
	focus(NULL);
	arrange(NULL);
}

void setclientstate(Client *c, long state) {
	long data[] = {state, None};

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
	                PropModeReplace, (unsigned char *) data, 2);
}

int sendevent(Client *c, Atom proto) {
	int n;
	Atom *protocols;
	int exists = 0;
	XEvent ev;

	if (XGetWMProtocols(dpy, c->win, &protocols, &n)) {
		while (!exists && n--)
			exists = protocols[n] == proto;
		XFree(protocols);
	}
	if (exists) {
		ev.type                 = ClientMessage;
		ev.xclient.window       = c->win;
		ev.xclient.message_type = wmatom[WMProtocols];
		ev.xclient.format       = 32;
		ev.xclient.data.l[0]    = proto;
		ev.xclient.data.l[1]    = CurrentTime;
		XSendEvent(dpy, c->win, False, NoEventMask, &ev);
	}
	return exists;
}

void setfocus(Client *c) {
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow], XA_WINDOW, 32,
		                PropModeReplace, (unsigned char *) &(c->win), 1);
	}
	sendevent(c, wmatom[WMTakeFocus]);
}

void setfullscreen(Client *c, int fullscreen) {
	if (fullscreen && !c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
		                PropModeReplace,
		                (unsigned char *) &netatom[NetWMFullscreen], 1);
		c->isfullscreen = 1;
		c->oldstate     = c->isfloating;
		c->oldbw        = c->bw;
		c->bw           = 0;
		c->isfloating   = 1;
		resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
		XRaiseWindow(dpy, c->win);
	} else if (!fullscreen && c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
		                PropModeReplace, (unsigned char *) 0, 0);
		c->isfullscreen = 0;
		c->isfloating   = c->oldstate;
		c->bw           = c->oldbw;
		c->x            = c->oldx;
		c->y            = c->oldy;
		c->w            = c->oldw;
		c->h            = c->oldh;
		resizeclient(c, c->x, c->y, c->w, c->h);
		arrange(c->mon);
	}
}

void setgaps(const Arg *arg) {
	if ((arg->i == 0) || (selmon->gappx + arg->i < 0))
		selmon->gappx = 0;
	else
		selmon->gappx += arg->i;
	arrange(selmon);
}

void switchgaps(const Arg *arg) {
	int n = cfg.ngapmodes;

	if (n == 0)
		return;
	selmon->gapidx = ((selmon->gapidx + (int) arg->i) % n + n) % n;
	selmon->gappx  = cfg.gapmodes[selmon->gapidx];

	arrange(selmon);
}

void setlayout(const Arg *arg) {
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = (Layout *) arg->v;
	snprintf(selmon->ltsymbol, sizeof selmon->ltsymbol, "%s",
	         selmon->lt[selmon->sellt]->symbol);
	if (selmon->sel)
		arrange(selmon);
	else
		drawbar(selmon);
}

void setlayoutsafe(const Arg *arg) {
	const Layout *ltptr = (Layout *) arg->v;
	size_t i;

	if (!ltptr) {
		setlayout(arg);
		return;
	}
	for (i = 0; i < cfg.nlayouts; i++)
		if (ltptr == &cfg.layouts[i]) {
			setlayout(arg);
			return;
		}
}

/* arg > 1.0 will set mfact absolutely */
void setmfact(const Arg *arg) {
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

/* (re)create fonts and color schemes from cfg; on failure the previous ones
 * stay in use and the reason is returned */
static const char *loadappearance(void) {
	Fnt *oldfonts = drw->fonts;
	Clr *s[SchemeLast];
	int i;

	for (i = 0; i < SchemeLast; i++)
		if (!(s[i] = drw_scm_create(drw, (const char **) cfg.colors[i], 3))) {
			while (i--)
				free(s[i]);
			return "cannot allocate colors";
		}
	if (!drw_fontset_create(drw, (const char **) cfg.fonts, cfg.nfonts)) {
		for (i = 0; i < SchemeLast; i++)
			free(s[i]);
		return "no fonts could be loaded";
	}
	drw_fontset_free(oldfonts);
	for (i = 0; i < SchemeLast; i++) {
		free(scheme[i]);
		scheme[i] = s[i];
	}
	lrpad = drw->fonts->h;
	bh    = drw->fonts->h + cfg.barpadding;
	return NULL;
}

static const Layout *findlayout(const char *name) {
	size_t i;

	for (i = 0; i < cfg.nlayouts; i++)
		if (!strcmp(cfg.layouts[i].name, name))
			return &cfg.layouts[i];
	return &cfg.layouts[0];
}

/* bring the running WM in line with cfg after a reload replaced old, which
 * is still valid during the call; NULL on success, else the reason */
const char *applyconfig(const Config *old) {
	const char *err;
	Monitor *m;
	Client *c;
	int bwchanged = cfg.borderpx != old->borderpx;

	if ((err = loadappearance()))
		return err;
	drw_resize(drw, sw, bh);
	for (m = mons; m; m = m->next) {
		/* keep runtime tweaks unless the configured value changed */
		if (cfg.mfact != old->mfact)
			m->mfact = cfg.mfact;
		if (cfg.nmaster != old->nmaster)
			m->nmaster = cfg.nmaster;
		if (cfg.showbar != old->showbar)
			m->showbar = cfg.showbar;
		if (cfg.topbar != old->topbar)
			m->topbar = cfg.topbar;
		if (cfg.gappx != old->gappx)
			m->gappx = cfg.gappx;
		m->gapidx = gapindex(m->gappx);
		/* layouts are owned by the configuration, find them by name */
		m->lt[0] = findlayout(m->lt[0]->name);
		m->lt[1] = findlayout(m->lt[1]->name);
		if (m->lastlt)
			m->lastlt = findlayout(m->lastlt->name);
		m->tagset[0] = m->tagset[0] & TAGMASK ? m->tagset[0] & TAGMASK : 1;
		m->tagset[1] = m->tagset[1] & TAGMASK ? m->tagset[1] & TAGMASK : 1;
		updatebarpos(m);
		XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, m->ww, bh);
		for (c = m->clients; c; c = c->next) {
			c->tags = c->tags & TAGMASK ? c->tags & TAGMASK
			                            : m->tagset[m->seltags];
			if (bwchanged && !c->isfullscreen) {
				c->bw = cfg.borderpx;
				resizeclient(c, c->x, c->y, c->w, c->h);
			}
			XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
			grabbuttons(c, 0);
		}
	}
	grabkeys();
	updatestatus();
	focus(NULL);
	arrange(NULL);
	return NULL;
}

void setup(void) {
	XSetWindowAttributes wa;
	Atom utf8string;
	struct sigaction sa;
	const char *err;

	/* do not transform children into zombies when they terminate */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags   = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);

	/* clean up any zombies (inherited from .xinitrc etc) immediately */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	/* init screen */
	screen = DefaultScreen(dpy);
	sw     = DisplayWidth(dpy, screen);
	sh     = DisplayHeight(dpy, screen);
	root   = RootWindow(dpy, screen);
	drw    = drw_create(dpy, screen, root, sw, sh);
	if ((err = loadappearance())) {
		/* the configuration asked for unusable fonts or colors */
		config_fallback(err);
		if ((err = loadappearance()))
			die("ewm: %s", err);
	}
	updategeom();
	/* init atoms */
	utf8string               = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMProtocols]      = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete]         = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState]          = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus]      = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported]    = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetWMName]       = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState]      = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMFullscreen] =
	    XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] =
	    XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	/* init cursors */
	cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
	cursor[CurResize] = drw_cur_create(drw, XC_sizing);
	cursor[CurMove]   = drw_cur_create(drw, XC_fleur);
	/* init bars */
	updatebars();
	updatestatus();
	/* supporting window for NetWMCheck */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
	                PropModeReplace, (unsigned char *) "ewm", 3);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	/* EWMH support per view */
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
	                PropModeReplace, (unsigned char *) netatom, NetLast);
	XDeleteProperty(dpy, root, netatom[NetClientList]);
	/* select events */
	wa.cursor     = cursor[CurNormal]->cursor;
	wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask
	              | ButtonPressMask | PointerMotionMask | EnterWindowMask
	              | LeaveWindowMask | StructureNotifyMask
	              | PropertyChangeMask;
	XChangeWindowAttributes(dpy, root, CWEventMask | CWCursor, &wa);
	XSelectInput(dpy, root, wa.event_mask);
	grabkeys();
	focus(NULL);
	setupepoll();
}

void setupepoll(void) {
	struct epoll_event dpy_event = {.events = EPOLLIN};
	char sockpath[sizeof(((struct sockaddr_un *) 0)->sun_path)];

	if ((epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1)
		die("epoll_create1:");
	dpy_fd            = ConnectionNumber(dpy);
	dpy_event.data.fd = dpy_fd;
	DEBUG("Display socket is fd %d\n", dpy_fd);
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, dpy_fd, &dpy_event))
		die("epoll_ctl: cannot add display fd:");

	/* the socket follows $DISPLAY; an inherited $EWM_SOCKET (e.g. from a
	 * nested session) must not redirect it. Children get our path. */
	unsetenv("EWM_SOCKET");
	if (ipc_socket_path(sockpath, sizeof(sockpath)) < 0
	    || ipc_init(sockpath, epoll_fd, ipccommands, LENGTH(ipccommands)) < 0)
		fputs("ewm: failed to initialize IPC\n", stderr);
	else
		setenv("EWM_SOCKET", sockpath, 1);
}

void seturgent(Client *c, int urg) {
	XWMHints *wmh;

	c->isurgent = urg;
	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	wmh->flags =
	    urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
	XSetWMHints(dpy, c->win, wmh);
	XFree(wmh);
}

void showhide(Client *c) {
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		/* show clients top down */
		XMoveWindow(dpy, c->win, c->x, c->y);
		if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating)
		    && !c->isfullscreen)
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		/* hide clients bottom up */
		showhide(c->snext);
		XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
	}
}

void sigstatusbar(const Arg *arg) {
	union sigval sv;

	if (!statussig || SIGRTMIN + statussig > SIGRTMAX)
		return;
	sv.sival_int = arg->i;
	if ((statuspid = getstatusbarpid()) <= 0)
		return;

	sigqueue(statuspid, SIGRTMIN + statussig, sv);
}

void spawn(const Arg *arg) {
	if (fork() == 0) {
		childsetup();
		execvp(((char **) arg->v)[0], (char **) arg->v);
		fprintf(stderr, "ewm: execvp %s", ((char **) arg->v)[0]);
		perror(" failed");
		exit(EXIT_FAILURE);
	}
}

void settags(Client *c, unsigned int tags) {
	if (!(tags & TAGMASK))
		return;
	c->tags = tags & TAGMASK;
	focus(NULL);
	arrange(c->mon);
}

void tag(const Arg *arg) {
	if (selmon->sel)
		settags(selmon->sel, arg->ui);
}

void tagmon(const Arg *arg) {
	if (!selmon->sel || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

void tile(Monitor *m) {
	unsigned int i, n, h, mw, my, ty;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++)
		;
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? m->ww * m->mfact : 0;
	else
		mw = m->ww - m->gappx;
	for (i = 0, my = ty = m->gappx, c = nexttiled(m->clients); c;
	     c = nexttiled(c->next), i++)
		if (i < m->nmaster) {
			h = (m->wh - my) / (MIN(n, m->nmaster) - i) - m->gappx;
			resize(c, m->wx + m->gappx, m->wy + my,
			       mw - (2 * c->bw) - m->gappx, h - (2 * c->bw), 0);
			if (my + HEIGHT(c) + m->gappx < m->wh)
				my += HEIGHT(c) + m->gappx;
		} else {
			h = (m->wh - ty) / (n - i) - m->gappx;
			resize(c, m->wx + mw + m->gappx, m->wy + ty,
			       m->ww - mw - (2 * c->bw) - 2 * m->gappx, h - (2 * c->bw),
			       0);
			if (ty + HEIGHT(c) + m->gappx < m->wh)
				ty += HEIGHT(c) + m->gappx;
		}
}

void togglebar(const Arg *arg) {
	selmon->showbar = !selmon->showbar;
	updatebarpos(selmon);
	XMoveResizeWindow(dpy, selmon->barwin, selmon->wx, selmon->by, selmon->ww,
	                  bh);
	arrange(selmon);
}

void setfloating(Client *c, int floating) {
	if (c->isfullscreen) /* no support for fullscreen windows */
		return;
	c->isfloating = floating || c->isfixed;
	if (c->isfloating)
		resize(c, c->x, c->y, c->w, c->h, 0);
	arrange(c->mon);
}

void togglefloating(const Arg *arg) {
	if (selmon->sel)
		setfloating(selmon->sel, !selmon->sel->isfloating);
}

void togglefullscr(const Arg *arg) {
	if (selmon->sel)
		setfullscreen(selmon->sel, !selmon->sel->isfullscreen);
}

void toggletag(const Arg *arg) {
	unsigned int newtags;

	if (!selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		focus(NULL);
		arrange(selmon);
	}
}

void toggleview(const Arg *arg) {
	unsigned int newtagset =
	    selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;
		focus(NULL);
		arrange(selmon);
	}
}

void unfocus(Client *c, int setfocus) {
	if (!c)
		return;
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
	if (setfocus) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

void unmanage(Client *c, int destroyed) {
	Monitor *m = c->mon;
	XWindowChanges wc;

	config_hook("unmanage", c);
	detach(c);
	detachstack(c);
	if (!destroyed) {
		wc.border_width = c->oldbw;
		XGrabServer(dpy); /* avoid race conditions */
		XSetErrorHandler(xerrordummy);
		XConfigureWindow(dpy, c->win, CWBorderWidth,
		                 &wc); /* restore border */
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	for (m = mons; m; m = m->next)
		if (m->lastsel == c)
			m->lastsel = NULL;
	if (hookfocused == c)
		hookfocused = NULL;
	m = c->mon;
	free(c);
	focus(NULL);
	updateclientlist();
	arrange(m);
}

void unmapnotify(XEvent *e) {
	Client *c;
	XUnmapEvent *ev = &e->xunmap;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	}
}

void updatebars(void) {
	Monitor *m;
	XSetWindowAttributes wa = {.override_redirect = True,
	                           .background_pixmap = ParentRelative,
	                           .event_mask = ButtonPressMask | ExposureMask};

	XClassHint ch = {"ewm", "ewm"};
	for (m = mons; m; m = m->next) {
		if (m->barwin)
			continue;
		m->barwin = XCreateWindow(
		    dpy, root, m->wx, m->by, m->ww, bh, 0, DefaultDepth(dpy, screen),
		    CopyFromParent, DefaultVisual(dpy, screen),
		    CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);
		XDefineCursor(dpy, m->barwin, cursor[CurNormal]->cursor);
		XMapRaised(dpy, m->barwin);
		XSetClassHint(dpy, m->barwin, &ch);
	}
}

void updatebarpos(Monitor *m) {
	m->wy = m->my;
	m->wh = m->mh;
	if (m->showbar) {
		m->wh -= bh;
		m->by = m->topbar ? m->wy : m->wy + m->wh;
		m->wy = m->topbar ? m->wy + bh : m->wy;
	} else
		m->by = -bh;
}

void updateclientlist() {
	Client *c;
	Monitor *m;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
			                PropModeAppend, (unsigned char *) &(c->win), 1);
}

int updategeom(void) {
	int dirty = 0;

#ifdef XINERAMA
	if (XineramaIsActive(dpy)) {
		int i, j, n, nn;
		Client *c;
		Monitor *m;
		XineramaScreenInfo *info   = XineramaQueryScreens(dpy, &nn);
		XineramaScreenInfo *unique = NULL;

		for (n = 0, m = mons; m; m = m->next, n++)
			;
		/* only consider unique geometries as separate screens */
		unique = ecalloc(nn, sizeof(XineramaScreenInfo));
		for (i = 0, j = 0; i < nn; i++)
			if (isuniquegeom(unique, j, &info[i]))
				memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
		XFree(info);
		nn = j;
		if (n <= nn) { /* new monitors available */
			for (i = 0; i < (nn - n); i++) {
				for (m = mons; m && m->next; m = m->next)
					;
				if (m)
					m->next = createmon();
				else
					mons = createmon();
			}
			for (i = 0, m = mons; i < nn && m; m = m->next, i++)
				if (i >= n || unique[i].x_org != m->mx
				    || unique[i].y_org != m->my || unique[i].width != m->mw
				    || unique[i].height != m->mh) {
					dirty  = 1;
					m->num = i;
					m->mx = m->wx = unique[i].x_org;
					m->my = m->wy = unique[i].y_org;
					m->mw = m->ww = unique[i].width;
					m->mh = m->wh = unique[i].height;
					updatebarpos(m);
				}
		} else { /* less monitors available nn < n */
			for (i = nn; i < n; i++) {
				for (m = mons; m && m->next; m = m->next)
					;
				while ((c = m->clients)) {
					dirty      = 1;
					m->clients = c->next;
					detachstack(c);
					c->mon = mons;
					attachtop(c);
					attachstack(c);
				}
				if (m == selmon)
					selmon = mons;
				cleanupmon(m);
			}
		}
		free(unique);
	} else
#endif /* XINERAMA */
	{  /* default monitor setup */
		if (!mons)
			mons = createmon();
		if (mons->mw != sw || mons->mh != sh) {
			dirty    = 1;
			mons->mw = mons->ww = sw;
			mons->mh = mons->wh = sh;
			updatebarpos(mons);
		}
	}
	if (dirty) {
		selmon = mons;
		selmon = wintomon(root);
	}
	return dirty;
}

void updatenumlockmask(void) {
	unsigned int i, j;
	XModifierKeymap *modmap;

	numlockmask = 0;
	modmap      = XGetModifierMapping(dpy);
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j]
			    == XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = (1 << i);
	XFreeModifiermap(modmap);
}

void updatesizehints(Client *c) {
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		/* size is uninitialized, ensure that size.flags aren't used */
		size.flags = PSize;
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & PAspect) {
		c->mina = (float) size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float) size.max_aspect.x / size.max_aspect.y;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed =
	    (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
}

/* width of stext; control characters separate clickable segments */
static void measurestatus(void) {
	char *text, *s, ch;

	statusw = 0;
	for (text = s = stext; *s; s++) {
		if ((unsigned char) (*s) < ' ') {
			ch = *s;
			*s = '\0';
			statusw += TEXTW(text) - lrpad;
			*s   = ch;
			text = s + 1;
		}
	}
	statusw += TEXTW(text) - lrpad + 2;
}

void updatestatus(void) {
	if (!gettextprop(root, XA_WM_NAME, stext, sizeof(stext)))
		strcpy(stext, "ewm-" VERSION);
	measurestatus();
	drawbar(selmon);
}

/* show text until the root window name changes again */
void setstatus(const char *text) {
	snprintf(stext, sizeof(stext), "%s", text);
	if (!drw || !selmon)
		return;
	measurestatus();
	drawbar(selmon);
}

void updatetitle(Client *c) {
	char oldname[sizeof(c->name)];
	strcpy(oldname, c->name);

	if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0') /* hack to mark broken clients */
		strcpy(c->name, broken);

	for (Monitor *m = mons; m; m = m->next) {
		if (m->sel == c && strcmp(oldname, c->name) != 0)
			ipc_focused_title_change_event(m->num, c->win, oldname, c->name);
	}
}

void updatewindowtype(Client *c) {
	Atom state = getatomprop(c, netatom[NetWMState]);
	Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

	if (state == netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == netatom[NetWMWindowTypeDialog])
		c->isfloating = 1;
}

void updatewmhints(Client *c) {
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		if (c == selmon->sel && wmh->flags & XUrgencyHint) {
			wmh->flags &= ~XUrgencyHint;
			XSetWMHints(dpy, c->win, wmh);
		} else
			c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
		if (wmh->flags & InputHint)
			c->neverfocus = !wmh->input;
		else
			c->neverfocus = 0;
		XFree(wmh);
	}
}

void view(const Arg *arg) {
	if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focus(NULL);
	arrange(selmon);
}

Client *wintoclient(Window w) {
	Client *c;
	Monitor *m;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			if (c->win == w)
				return c;
	return NULL;
}

Monitor *wintomon(Window w) {
	int x, y;
	Client *c;
	Monitor *m;

	if (w == root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	for (m = mons; m; m = m->next)
		if (w == m->barwin)
			return m;
	if ((c = wintoclient(w)))
		return c->mon;
	return selmon;
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's). Other types of errors call Xlibs
 * default error handler, which may call exit. */
int xerror(Display *dpy, XErrorEvent *ee) {
	if (ee->error_code == BadWindow
	    || (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	    || (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
	    || (ee->request_code == X_PolyFillRectangle
	        && ee->error_code == BadDrawable)
	    || (ee->request_code == X_PolySegment
	        && ee->error_code == BadDrawable)
	    || (ee->request_code == X_ConfigureWindow
	        && ee->error_code == BadMatch)
	    || (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	    || (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
	    || (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
	fprintf(stderr, "ewm: fatal error: request code=%d, error code=%d\n",
	        ee->request_code, ee->error_code);
	return xerrorxlib(dpy, ee); /* may call exit */
}

int xerrordummy(Display *dpy, XErrorEvent *ee) {
	return 0;
}

/* Startup Error handler to check if another window manager
 * is already running. */
int xerrorstart(Display *dpy, XErrorEvent *ee) {
	die("ewm: another window manager is already running");
	return -1;
}

void zoom(const Arg *arg) {
	Client *c = selmon->sel;

	if (!selmon->lt[selmon->sellt]->arrange
	    || (selmon->sel && selmon->sel->isfloating))
		return;
	if (c == nexttiled(selmon->clients))
		if (!c || !(c = nexttiled(c->next)))
			return;
	pop(c);
}

int main(int argc, char *argv[]) {
	if (argc == 2 && !strcmp("-v", argv[1]))
		die("ewm-" VERSION);
	else if (argc != 1)
		die("usage: ewm [-v]");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("ewm: cannot open display");
	checkotherwm();
	config_init();
	setup();
	scan();
	runautostart();
	config_start();
	run();
	cleanup();
	config_cleanup();
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
