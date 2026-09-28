/* See LICENSE file for copyright and license details.
 *
 * Types and functions shared by the ewm core (ewm.c), the Lua configuration
 * (config.c) and the IPC server (ipc.c).
 */
#ifndef EWM_H_
#define EWM_H_

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <stddef.h>

#include "drw.h"

#define LENGTH(X)            (sizeof X / sizeof X[0])
#define ISVISIBLEONTAG(C, T) (((C)->tags & (T)))
#define ISVISIBLE(C)         ISVISIBLEONTAG(C, C->mon->tagset[C->mon->seltags])
#define WIDTH(X)             ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)            ((X)->h + 2 * (X)->bw)
#define TAGMASK              ((1u << cfg.ntags) - 1)
#define MAXTAGS              31 /* tags must fit into an unsigned int */

enum { SchemeNorm, SchemeSel, SchemeLast }; /* color schemes */
enum {
	ClkTagBar,
	ClkLtSymbol,
	ClkStatusText,
	ClkWinTitle,
	ClkClientWin,
	ClkRootWin,
	ClkLast
}; /* clicks */

typedef struct TagState TagState;
struct TagState {
	int selected;
	int occupied;
	int urgent;
};

typedef struct ClientState ClientState;
struct ClientState {
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen;
};

typedef union {
	long i;
	unsigned long ui;
	float f;
	const void *v;
} Arg;

typedef struct Monitor Monitor;
typedef struct Client Client;
struct Client {
	char name[256];
	float mina, maxa;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh;
	int bw, oldbw;
	unsigned int tags;
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen;
	Client *next;
	Client *snext;
	Monitor *mon;
	Window win;
	ClientState prevstate;
};

/* key and button bindings either call a C action with arg, or, when func is
 * config_call, the Lua callback stored under registry reference arg.i */
typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	Arg arg;
} Key;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	Arg arg;
} Button;

typedef struct {
	char *name;   /* stable identifier used by the configuration */
	char *symbol; /* shown in the bar */
	void (*arrange)(Monitor *);
	int ref;      /* Lua arrange function for Lua layouts */
} Layout;

struct Monitor {
	char ltsymbol[16];
	char lastltsymbol[16];
	float mfact;
	int nmaster;
	int num;
	int by;             /* bar geometry */
	int mx, my, mw, mh; /* screen size */
	int wx, wy, ww, wh; /* window area  */
	int gappx;          /* gaps between windows */
	int gapidx;         /* gap mode index */
	unsigned int seltags;
	unsigned int sellt;
	unsigned int tagset[2];
	TagState tagstate;
	int showbar;
	int topbar;
	Client *clients;
	Client *sel;
	Client *lastsel;
	Client *stack;
	Monitor *next;
	Window barwin;
	const Layout *lt[2];
	const Layout *lastlt;
};

typedef struct {
	char *class;
	char *instance;
	char *title;
	unsigned int tags;
	int isfloating;
	int monitor;
} Rule;

/* everything that can be set from config.lua; replaced as a whole on reload */
typedef struct {
	unsigned int borderpx;   /* border pixel of windows */
	unsigned int snap;       /* snap pixel */
	unsigned int gappx;      /* gaps between windows */
	unsigned int *gapmodes;  /* gap sizes cycled by switchgaps */
	size_t ngapmodes;
	int showbar;             /* 0 means no bar */
	int topbar;              /* 0 means bottom bar */
	int barpadding;          /* extra bar height in pixels */
	float mfact;             /* factor of master area size [0.05..0.95] */
	int nmaster;             /* number of clients in master area */
	int resizehints;         /* 1 means respect size hints in tiled resizals */
	int autoreload;          /* reload when the configuration changes */
	char **fonts;
	size_t nfonts;
	char *colors[SchemeLast][3]; /* fg, bg, border */
	char **tags;
	size_t ntags;
	Layout *layouts;         /* first entry is the default */
	size_t nlayouts;
	Rule *rules;
	size_t nrules;
	Key *keys;
	size_t nkeys;
	Button *buttons;
	size_t nbuttons;
} Config;

/* ewm.c state */
extern Config cfg;
extern Display *dpy;
extern Window root;
extern Drw *drw;
extern Monitor *mons, *selmon, *lastselmon;
extern int epoll_fd;

/* ewm.c actions, bound to keys, buttons, Lua and IPC */
void focusmon(const Arg *arg);
void focusstack(const Arg *arg);
void incnmaster(const Arg *arg);
void killclient(const Arg *arg);
void moveresize(const Arg *arg);
void moveresizeedge(const Arg *arg);
void movemouse(const Arg *arg);
void quit(const Arg *arg);
void reload(const Arg *arg);
void resizemouse(const Arg *arg);
void setgaps(const Arg *arg);
void setlayout(const Arg *arg);
void setlayoutsafe(const Arg *arg);
void setmfact(const Arg *arg);
void spawn(const Arg *arg);
void switchgaps(const Arg *arg);
void tag(const Arg *arg);
void tagmon(const Arg *arg);
void togglebar(const Arg *arg);
void togglefloating(const Arg *arg);
void togglefullscr(const Arg *arg);
void toggletag(const Arg *arg);
void toggleview(const Arg *arg);
void view(const Arg *arg);
void zoom(const Arg *arg);

/* ewm.c helpers */
extern const Layout builtinlayouts[];
extern const size_t nbuiltinlayouts;
const char *applyconfig(const Config *old); /* NULL or reason of failure */
void arrange(Monitor *m);
void closeclient(Client *c);
void drawbars(void);
void focus(Client *c);
void grabbuttons(Client *c, int focused);
void grabkeys(void);
Client *nexttiled(Client *c);
void resize(Client *c, int x, int y, int w, int h, int interact);
void restack(Monitor *m);
void setfloating(Client *c, int floating);
void setfullscreen(Client *c, int fullscreen);
void setstatus(const char *text);
void settags(Client *c, unsigned int tags);
Client *wintoclient(Window w);

/* config.c */
void config_call(const Arg *arg);   /* run the Lua callback of a key */
void config_callbutton(const Arg *arg, int tag); /* same for a button */
void config_cleanup(void);
void config_fallback(const char *reason); /* switch to built-in defaults */
int config_handlefd(int fd);        /* 1 if fd belongs to the configuration */
void config_hook(const char *event, Client *c);
void config_init(void);             /* load config.lua, else built-in defaults */
void config_layout(Monitor *m);     /* arrange function of Lua layouts */
void config_reload(void);
void config_start(void);            /* once the WM runs: watches, timers */

#endif /* EWM_H_ */
