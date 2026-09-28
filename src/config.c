/* See LICENSE file for copyright and license details.
 *
 * Lua configuration. config.lua (and every .lua file in plugins/ next to it) is run
 * in a fresh Lua state that talks to ewm through the `ewm` module. Settings
 * and bindings are collected into a Config; only if everything loads and
 * applies does it replace the running one, so a broken edit never leaves the
 * WM half configured. Bindings, hooks, layouts and timers hold references
 * into the Lua state they came from, which is closed when it is replaced.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <lualib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "ewm.h"
#include "util.h"

#ifndef DATADIR
#define DATADIR "/usr/local/share/ewm"
#endif

#define HOOKS     "ewm.hooks" /* registry: event name -> list of functions */
#define AUTOSTART "ewm.autostart" /* registry: commands for config_start */
#define DEBOUNCE  150000000L  /* ns to wait for more changes before reload */
#define WATCHMASK                                                            \
	(IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE)

typedef struct Timer Timer;
struct Timer {
	int fd;
	int id;
	int ref;           /* callback in owner's registry */
	int repeat;
	lua_State *owner;
	Timer *next;
};

enum { ANone, AInt, AFloat, ATags, AString, ALayout, ASpawn };
typedef struct {
	const char *name;
	void (*func)(const Arg *);
	int argtype;
} Action;

static const Action actions[] = {
    {"focusmon", focusmon, AInt},
    {"focusstack", focusstack, AInt},
    {"incnmaster", incnmaster, AInt},
    {"killclient", killclient, ANone},
    {"moveresize", moveresize, AString},
    {"moveresizeedge", moveresizeedge, AString},
    {"movemouse", movemouse, ANone},
    {"quit", quit, ANone},
    {"reload", reload, ANone},
    {"resizemouse", resizemouse, ANone},
    {"setgaps", setgaps, AInt},
    {"setlayout", setlayout, ALayout},
    {"setmfact", setmfact, AFloat},
    {"spawn", spawn, ASpawn},
    {"switchgaps", switchgaps, AInt},
    {"tag", tag, ATags},
    {"tagmon", tagmon, AInt},
    {"togglebar", togglebar, ANone},
    {"togglefloating", togglefloating, ANone},
    {"togglefullscreen", togglefullscr, ANone},
    {"toggletag", toggletag, ATags},
    {"toggleview", toggleview, ATags},
    {"view", view, ATags},
    {"zoom", zoom, ANone},
};

static const char *hooknames[] = {"startup", "manage", "unmanage", "focus"};

static const struct {
	const char *name;
	unsigned int click;
} clicks[] = {
    {"tagbar", ClkTagBar},    {"layout", ClkLtSymbol}, {"status", ClkStatusText},
    {"title", ClkWinTitle},   {"client", ClkClientWin}, {"root", ClkRootWin},
};

static lua_State *L;     /* state of the active configuration */
static lua_State *loadL; /* state being loaded, owner of loadcfg */
static Config *loadcfg;
static char **layoutorder; /* ewm.set{layouts = ...} of the loading state */
static size_t nlayoutorder;
static char cfgdir[PATH_MAX]; /* directory of the active config.lua */
static char errbuf[1024];
static Timer *timers;
static int timerseq;
static int inotifyfd = -1, debouncefd = -1;
static int started; /* config_start() ran; epoll and X are usable */
static int hookdepth;

static void *xrealloc(void *p, size_t size) {
	if (!(p = realloc(p, size)))
		die("realloc:");
	return p;
}

static char *xstrdup(const char *s) {
	char *d = ecalloc(strlen(s) + 1, 1);

	return strcpy(d, s);
}

#define PUSH(arr, n, item)                                                   \
	do {                                                                     \
		(arr)        = xrealloc((arr), ((n) + 1) * sizeof(*(arr)));          \
		(arr)[(n)++] = (item);                                               \
	} while (0)

/* print and show in the bar */
static void report(const char *fmt, ...) {
	char msg[sizeof(errbuf) - sizeof("ewm: ") + 1], *nl;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(stderr, "ewm: %s\n", msg);
	/* control characters separate status segments; keep the first line */
	if ((nl = strchr(msg, '\n')))
		*nl = '\0';
	snprintf(errbuf, sizeof(errbuf), "ewm: %s", msg);
	if (started)
		setstatus(errbuf);
}

static lua_State *mainstate(lua_State *l) {
	lua_State *m;

	lua_rawgeti(l, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
	m = lua_tothread(l, -1);
	lua_pop(l, 1);
	return m;
}

/* configuration that bindings registered from l belong to */
static Config *target(lua_State *l) {
	return mainstate(l) == loadL ? loadcfg : &cfg;
}

static int traceback(lua_State *l) {
	luaL_traceback(l, l, lua_tostring(l, 1), 1);
	return 1;
}

/* call the function below nargs arguments; on error report it */
static int pcall(lua_State *l, int nargs, int nresults, const char *what) {
	int base = lua_gettop(l) - nargs, res;

	lua_pushcfunction(l, traceback);
	lua_insert(l, base);
	res = lua_pcall(l, nargs, nresults, base);
	lua_remove(l, base);
	if (res != LUA_OK) {
		report("%s: %s", what, lua_tostring(l, -1));
		lua_pop(l, 1);
		return -1;
	}
	return 0;
}

/* configuration data */

static void freestrv(char **v, size_t n) {
	while (n--)
		free(v[n]);
	free(v);
}

void config_free(Config *c) {
	size_t i;
	int s, j;

	free(c->gapmodes);
	freestrv(c->fonts, c->nfonts);
	for (s = 0; s < SchemeLast; s++)
		for (j = 0; j < 3; j++)
			free(c->colors[s][j]);
	freestrv(c->tags, c->ntags);
	for (i = 0; i < c->nlayouts; i++) {
		free(c->layouts[i].name);
		free(c->layouts[i].symbol);
	}
	free(c->layouts);
	for (i = 0; i < c->nrules; i++) {
		free(c->rules[i].class);
		free(c->rules[i].instance);
		free(c->rules[i].title);
	}
	free(c->rules);
	free(c->keys);
	free(c->buttons);
	memset(c, 0, sizeof(*c));
}

static void addlayout(Config *c, const char *name, const char *symbol,
                      void (*arrange)(Monitor *), int ref) {
	Layout l = {xstrdup(name), xstrdup(symbol), arrange, ref};

	PUSH(c->layouts, c->nlayouts, l);
}

/* settings used when config.lua does not override them */
static void setdefaults(Config *c) {
	static const char *colors[SchemeLast][3] = {
	    [SchemeNorm] = {"#bbbbbb", "#222222", "#444444"},
	    [SchemeSel]  = {"#eeeeee", "#005577", "#005577"},
	};
	static const unsigned int gapmodes[] = {5, 0};
	char name[3];
	size_t i;
	int s, j;

	memset(c, 0, sizeof(*c));
	c->borderpx    = 1;
	c->snap        = 32;
	c->gappx       = 5;
	c->showbar     = 1;
	c->topbar      = 1;
	c->barpadding  = 2;
	c->mfact       = 0.55;
	c->nmaster     = 1;
	c->resizehints = 1;
	c->autoreload  = 1;
	for (i = 0; i < LENGTH(gapmodes); i++)
		PUSH(c->gapmodes, c->ngapmodes, gapmodes[i]);
	PUSH(c->fonts, c->nfonts, xstrdup("monospace:size=10"));
	for (s = 0; s < SchemeLast; s++)
		for (j = 0; j < 3; j++)
			c->colors[s][j] = xstrdup(colors[s][j]);
	for (i = 1; i <= 9; i++) {
		snprintf(name, sizeof(name), "%zu", i);
		PUSH(c->tags, c->ntags, xstrdup(name));
	}
	for (i = 0; i < nbuiltinlayouts; i++)
		addlayout(c, builtinlayouts[i].name, builtinlayouts[i].symbol,
		          builtinlayouts[i].arrange, LUA_NOREF);
}

/* minimal bindings for when config.lua is missing or broken */
static void setfallbackbindings(Config *c) {
	static const char *termcmd[] = {"/bin/sh", "-c", "exec ${TERMINAL:-xterm}",
	                                NULL};
	const struct {
		unsigned int mod;
		KeySym keysym;
		void (*func)(const Arg *);
		Arg arg;
	} keys[] = {
	    {Mod4Mask, XK_Return, spawn, {.v = termcmd}},
	    {Mod4Mask, XK_j, focusstack, {.i = +1}},
	    {Mod4Mask, XK_k, focusstack, {.i = -1}},
	    {Mod4Mask, XK_space, setlayout, {0}},
	    {Mod4Mask, XK_b, togglebar, {0}},
	    {Mod4Mask | ShiftMask, XK_q, killclient, {0}},
	    {Mod4Mask | ShiftMask, XK_r, reload, {0}},
	    {Mod4Mask | ShiftMask, XK_e, quit, {0}},
	};
	const Button buttons[] = {
	    {ClkClientWin, Mod4Mask, Button1, movemouse, {0}},
	    {ClkClientWin, Mod4Mask, Button3, resizemouse, {0}},
	    {ClkTagBar, 0, Button1, view, {0}},
	};
	size_t i;

	for (i = 0; i < LENGTH(keys); i++) {
		Key k = {keys[i].mod, keys[i].keysym, keys[i].func, keys[i].arg};

		PUSH(c->keys, c->nkeys, k);
	}
	for (i = 0; i < 9; i++) {
		Key v = {Mod4Mask, XK_1 + i, view, {.ui = 1 << i}};
		Key t = {Mod4Mask | ShiftMask, XK_1 + i, tag, {.ui = 1 << i}};

		PUSH(c->keys, c->nkeys, v);
		PUSH(c->keys, c->nkeys, t);
	}
	for (i = 0; i < LENGTH(buttons); i++)
		PUSH(c->buttons, c->nbuttons, buttons[i]);
}

/* argument conversion */

static unsigned int checktags(lua_State *l, int idx) {
	unsigned int mask = 0;
	lua_Integer n;
	int i, len;

	if (lua_type(l, idx) == LUA_TSTRING
	    && !strcmp(lua_tostring(l, idx), "all"))
		return ~0u;
	if (lua_istable(l, idx)) {
		len = luaL_len(l, idx);
		for (i = 1; i <= len; i++) {
			lua_rawgeti(l, idx, i);
			mask |= checktags(l, lua_gettop(l));
			lua_pop(l, 1);
		}
		return mask;
	}
	n = luaL_checkinteger(l, idx);
	if (n == 0)
		return ~0u;
	if (n < 1 || n > MAXTAGS)
		luaL_error(l, "tag %d out of range", (int) n);
	return 1u << (n - 1);
}

static void pushtags(lua_State *l, unsigned int mask) {
	int i, n = 0;

	lua_newtable(l);
	for (i = 0; i < MAXTAGS; i++)
		if (mask & 1u << i) {
			lua_pushinteger(l, i + 1);
			lua_rawseti(l, -2, ++n);
		}
}

static unsigned int checkmods(lua_State *l, int idx) {
	static const struct {
		const char *name;
		unsigned int mask;
	} mods[] = {
	    {"Shift", ShiftMask}, {"Control", ControlMask}, {"Ctrl", ControlMask},
	    {"Lock", LockMask},   {"Mod1", Mod1Mask},       {"Alt", Mod1Mask},
	    {"Mod2", Mod2Mask},   {"Mod3", Mod3Mask},       {"Mod4", Mod4Mask},
	    {"Super", Mod4Mask},  {"Mod5", Mod5Mask},
	};
	const char *s, *e;
	unsigned int mask = 0;
	size_t i, len;
	int j, n;

	if (lua_isnoneornil(l, idx))
		return 0;
	if (lua_istable(l, idx)) {
		n = luaL_len(l, idx);
		for (j = 1; j <= n; j++) {
			lua_rawgeti(l, idx, j);
			mask |= checkmods(l, lua_gettop(l));
			lua_pop(l, 1);
		}
		return mask;
	}
	/* "Mod4+Shift" */
	for (s = luaL_checkstring(l, idx); *s; s = *e ? e + 1 : e) {
		e   = strchr(s, '+') ? strchr(s, '+') : s + strlen(s);
		len = e - s;
		for (i = 0; i < LENGTH(mods); i++)
			if (strlen(mods[i].name) == len && !strncmp(s, mods[i].name, len))
				break;
		if (len && i == LENGTH(mods))
			luaL_error(l, "unknown modifier '%.*s'", (int) len, s);
		if (len)
			mask |= mods[i].mask;
	}
	return mask;
}

static Client *checkclient(lua_State *l, int idx) {
	Client *c = wintoclient((Window) luaL_checkinteger(l, idx));

	if (!c)
		luaL_error(l, "no client with id %d", (int) lua_tointeger(l, idx));
	return c;
}

static void setfield(lua_State *l, const char *k, lua_Integer v) {
	lua_pushinteger(l, v);
	lua_setfield(l, -2, k);
}

static void pushclient(lua_State *l, Client *c) {
	XClassHint ch = {NULL, NULL};

	if (!c) {
		lua_pushnil(l);
		return;
	}
	lua_newtable(l);
	setfield(l, "id", c->win);
	lua_pushstring(l, c->name);
	lua_setfield(l, -2, "name");
	if (XGetClassHint(dpy, c->win, &ch)) {
		lua_pushstring(l, ch.res_class ? ch.res_class : "");
		lua_setfield(l, -2, "class");
		lua_pushstring(l, ch.res_name ? ch.res_name : "");
		lua_setfield(l, -2, "instance");
		if (ch.res_class)
			XFree(ch.res_class);
		if (ch.res_name)
			XFree(ch.res_name);
	}
	pushtags(l, c->tags);
	lua_setfield(l, -2, "tags");
	lua_pushboolean(l, c->isfloating);
	lua_setfield(l, -2, "floating");
	lua_pushboolean(l, c->isfullscreen);
	lua_setfield(l, -2, "fullscreen");
	lua_pushboolean(l, c->isurgent);
	lua_setfield(l, -2, "urgent");
	setfield(l, "monitor", c->mon->num);
	setfield(l, "x", c->x);
	setfield(l, "y", c->y);
	setfield(l, "w", c->w);
	setfield(l, "h", c->h);
}

/* ewm.* functions */

static void needwm(lua_State *l, const char *name) {
	if (!selmon)
		luaL_error(l, "ewm.%s: the window manager is not running yet", name);
}

static int l_action(lua_State *l) {
	const Action *a = lua_touserdata(l, lua_upvalueindex(1));
	const char *argv[64];
	Arg arg = {0};
	size_t i;
	int n;

	if (a->func != spawn)
		needwm(l, a->name);
	switch (a->argtype) {
	case AInt:
		arg.i = luaL_checkinteger(l, 1);
		break;
	case AFloat:
		arg.f = luaL_checknumber(l, 1);
		break;
	case ATags: /* view() without a tag returns to the previous tagset */
		arg.ui = a->func == view && lua_isnoneornil(l, 1) ? 0 : checktags(l, 1);
		break;
	case AString:
		arg.v = luaL_checkstring(l, 1);
		break;
	case ALayout:
		if (lua_isnoneornil(l, 1))
			break;
		for (i = 0; i < cfg.nlayouts; i++)
			if (!strcmp(cfg.layouts[i].name, luaL_checkstring(l, 1)))
				arg.v = &cfg.layouts[i];
		if (!arg.v)
			return luaL_error(l, "unknown layout '%s'", lua_tostring(l, 1));
		break;
	case ASpawn: /* a shell command line or an argv table */
		if (lua_istable(l, 1)) {
			n = luaL_len(l, 1);
			luaL_argcheck(l, n > 0 && n < (int) LENGTH(argv), 1,
			              "argv must have 1-63 entries");
			for (i = 0; i < (size_t) n; i++) {
				lua_rawgeti(l, 1, i + 1);
				argv[i] = luaL_checkstring(l, -1);
			}
			argv[n] = NULL;
		} else {
			argv[0] = "/bin/sh";
			argv[1] = "-c";
			argv[2] = luaL_checkstring(l, 1);
			argv[3] = NULL;
		}
		arg.v = argv;
		break;
	}
	if (a->func == reload && loadL)
		return 0; /* reloading from config.lua itself would never end */
	a->func(&arg);
	return 0;
}

static void setint(lua_State *l, const char *k, int *dst, int min, int max) {
	lua_Integer v = luaL_checkinteger(l, -1);

	if (v < min || v > max)
		luaL_error(l, "%s must be between %d and %d", k, min, max);
	*dst = v;
}

static void setstrv(lua_State *l, const char *k, char ***v, size_t *n,
                    size_t max) {
	char **nv = NULL;
	size_t nn = 0, i, len;

	if (lua_isstring(l, -1)) {
		PUSH(nv, nn, xstrdup(lua_tostring(l, -1)));
	} else {
		luaL_checktype(l, -1, LUA_TTABLE);
		len = luaL_len(l, -1);
		if (len < 1 || len > max) {
			luaL_error(l, "%s needs 1 to %d entries", k, (int) max);
		}
		for (i = 1; i <= len; i++) {
			lua_rawgeti(l, -1, i);
			if (!lua_isstring(l, -1)) {
				freestrv(nv, nn);
				luaL_error(l, "%s[%d] must be a string", k, (int) i);
			}
			PUSH(nv, nn, xstrdup(lua_tostring(l, -1)));
			lua_pop(l, 1);
		}
	}
	freestrv(*v, *n);
	*v = nv;
	*n = nn;
}

static void setcolors(lua_State *l, Config *c) {
	static const char *names[SchemeLast] = {"norm", "sel"};
	int s, j;

	luaL_checktype(l, -1, LUA_TTABLE);
	for (s = 0; s < SchemeLast; s++) {
		if (lua_getfield(l, -1, names[s]) == LUA_TNIL) {
			lua_pop(l, 1);
			continue;
		}
		luaL_checktype(l, -1, LUA_TTABLE);
		for (j = 0; j < 3; j++) {
			lua_rawgeti(l, -1, j + 1);
			if (!lua_isstring(l, -1))
				luaL_error(l, "colors.%s needs {fg, bg, border}", names[s]);
			free(c->colors[s][j]);
			c->colors[s][j] = xstrdup(lua_tostring(l, -1));
			lua_pop(l, 1);
		}
		lua_pop(l, 1);
	}
}

/* ewm.set{key = value, ...} */
static int l_set(lua_State *l) {
	Config *c = target(l);
	const char *k;
	size_t i, n;
	int v;

	luaL_checktype(l, 1, LUA_TTABLE);
	lua_pushnil(l);
	while (lua_next(l, 1)) {
		if (lua_type(l, -2) != LUA_TSTRING)
			return luaL_error(l, "setting names must be strings");
		k = lua_tostring(l, -2);
		if (!strcmp(k, "borderpx")) {
			setint(l, k, &v, 0, 100);
			c->borderpx = v;
		} else if (!strcmp(k, "snap")) {
			setint(l, k, &v, 0, 10000);
			c->snap = v;
		} else if (!strcmp(k, "gappx")) {
			setint(l, k, &v, 0, 1000);
			c->gappx = v;
		} else if (!strcmp(k, "barpadding"))
			setint(l, k, &c->barpadding, 0, 1000);
		else if (!strcmp(k, "nmaster"))
			setint(l, k, &c->nmaster, 0, 1000);
		else if (!strcmp(k, "showbar"))
			c->showbar = lua_toboolean(l, -1);
		else if (!strcmp(k, "topbar"))
			c->topbar = lua_toboolean(l, -1);
		else if (!strcmp(k, "resizehints"))
			c->resizehints = lua_toboolean(l, -1);
		else if (!strcmp(k, "autoreload"))
			c->autoreload = lua_toboolean(l, -1);
		else if (!strcmp(k, "mfact")) {
			c->mfact = luaL_checknumber(l, -1);
			if (c->mfact < 0.05 || c->mfact > 0.95)
				return luaL_error(l, "mfact must be between 0.05 and 0.95");
		} else if (!strcmp(k, "fonts"))
			setstrv(l, k, &c->fonts, &c->nfonts, 32);
		else if (!strcmp(k, "tags"))
			setstrv(l, k, &c->tags, &c->ntags, MAXTAGS);
		else if (!strcmp(k, "layouts"))
			setstrv(l, k, &layoutorder, &nlayoutorder, 64);
		else if (!strcmp(k, "colors"))
			setcolors(l, c);
		else if (!strcmp(k, "gapmodes")) {
			luaL_checktype(l, -1, LUA_TTABLE);
			free(c->gapmodes);
			c->gapmodes  = NULL;
			c->ngapmodes = 0;
			n            = luaL_len(l, -1);
			for (i = 1; i <= n; i++) {
				lua_rawgeti(l, -1, i);
				setint(l, "gapmodes[]", &v, 0, 1000);
				PUSH(c->gapmodes, c->ngapmodes, (unsigned int) v);
				lua_pop(l, 1);
			}
		} else
			return luaL_error(l, "unknown setting '%s'", k);
		lua_pop(l, 1);
	}
	return 0;
}

/* store the function at idx and the arguments after it for a binding */
static int bindref(lua_State *l, int idx) {
	int i, top = lua_gettop(l);

	luaL_checktype(l, idx, LUA_TFUNCTION);
	lua_createtable(l, top - idx + 1, 0);
	for (i = idx; i <= top; i++) {
		lua_pushvalue(l, i);
		lua_rawseti(l, -2, i - idx + 1);
	}
	return luaL_ref(l, LUA_REGISTRYINDEX);
}

/* ewm.key(mods, keysym, fn, args...) */
static int l_key(lua_State *l) {
	Config *c = target(l);
	Key k;

	k.mod    = checkmods(l, 1);
	k.keysym = XStringToKeysym(luaL_checkstring(l, 2));
	if (k.keysym == NoSymbol)
		return luaL_error(l, "unknown key '%s'", lua_tostring(l, 2));
	k.func  = config_call;
	k.arg.i = bindref(l, 3);
	PUSH(c->keys, c->nkeys, k);
	if (c == &cfg && selmon)
		grabkeys();
	return 0;
}

/* ewm.button(click, mods, button, fn, args...) */
static int l_button(lua_State *l) {
	Config *c = target(l);
	const char *click = luaL_checkstring(l, 1);
	Button b;
	Monitor *m;
	Client *cl;
	size_t i;

	for (i = 0; i < LENGTH(clicks) && strcmp(clicks[i].name, click); i++)
		;
	if (i == LENGTH(clicks))
		return luaL_error(l, "unknown click target '%s'", click);
	b.click  = clicks[i].click;
	b.mask   = checkmods(l, 2);
	b.button = luaL_checkinteger(l, 3);
	b.func   = config_call;
	b.arg.i  = bindref(l, 4);
	PUSH(c->buttons, c->nbuttons, b);
	if (c == &cfg && selmon)
		for (m = mons; m; m = m->next)
			for (cl = m->clients; cl; cl = cl->next)
				grabbuttons(cl, cl == selmon->sel);
	return 0;
}

static char *optfield(lua_State *l, const char *k) {
	char *s = NULL;

	if (lua_getfield(l, 1, k) != LUA_TNIL)
		s = xstrdup(luaL_checkstring(l, -1));
	lua_pop(l, 1);
	return s;
}

/* ewm.rule{class=, instance=, title=, tags=, floating=, monitor=} */
static int l_rule(lua_State *l) {
	Config *c = target(l);
	Rule r    = {0};

	luaL_checktype(l, 1, LUA_TTABLE);
	r.monitor = -1;
	if (lua_getfield(l, 1, "tags") != LUA_TNIL)
		r.tags = checktags(l, lua_gettop(l));
	lua_pop(l, 1);
	lua_getfield(l, 1, "floating");
	r.isfloating = lua_toboolean(l, -1);
	lua_pop(l, 1);
	if (lua_getfield(l, 1, "monitor") != LUA_TNIL)
		r.monitor = luaL_checkinteger(l, -1);
	lua_pop(l, 1);
	r.class    = optfield(l, "class");
	r.instance = optfield(l, "instance");
	r.title    = optfield(l, "title");
	PUSH(c->rules, c->nrules, r);
	return 0;
}

/* ewm.layout(name, symbol, fn): fn(area, n) returns n {x, y, w, h} boxes
 * (including borders) for the tiled clients of a monitor */
static int l_layout(lua_State *l) {
	Config *c        = target(l);
	const char *name = luaL_checkstring(l, 1);
	const char *sym  = luaL_checkstring(l, 2);
	size_t i;

	luaL_checktype(l, 3, LUA_TFUNCTION);
	for (i = 0; i < c->nlayouts; i++)
		if (!strcmp(c->layouts[i].name, name))
			return luaL_error(l, "layout '%s' already exists", name);
	lua_pushvalue(l, 3);
	addlayout(c, name, sym, config_layout, luaL_ref(l, LUA_REGISTRYINDEX));
	return 0;
}

/* ewm.on(event, fn) */
static int l_on(lua_State *l) {
	const char *ev = luaL_checkstring(l, 1);
	size_t i;

	luaL_checktype(l, 2, LUA_TFUNCTION);
	for (i = 0; i < LENGTH(hooknames) && strcmp(hooknames[i], ev); i++)
		;
	if (i == LENGTH(hooknames))
		return luaL_error(l, "unknown event '%s'", ev);
	lua_getfield(l, LUA_REGISTRYINDEX, HOOKS);
	if (lua_getfield(l, -1, ev) == LUA_TNIL) {
		lua_pop(l, 1);
		lua_newtable(l);
		lua_pushvalue(l, -1);
		lua_setfield(l, -3, ev);
	}
	lua_pushvalue(l, 2);
	lua_rawseti(l, -2, luaL_len(l, -2) + 1);
	return 0;
}

static void armtimer(Timer *t) {
	/* zero all of data: only its fd member is set */
	struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0};

	ev.data.fd = t->fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, t->fd, &ev) < 0)
		fprintf(stderr, "ewm: cannot watch timer: %s\n", strerror(errno));
}

static void freetimer(Timer *t) {
	Timer **tp;

	for (tp = &timers; *tp && *tp != t; tp = &(*tp)->next)
		;
	if (*tp)
		*tp = t->next;
	if (started)
		epoll_ctl(epoll_fd, EPOLL_CTL_DEL, t->fd, NULL);
	close(t->fd);
	free(t);
}

/* ewm.timer(seconds, fn[, repeat]) -> id */
static int l_timer(lua_State *l) {
	double secs = luaL_checknumber(l, 1);
	struct itimerspec its = {{0, 0}, {0, 0}};
	Timer *t;

	luaL_argcheck(l, secs > 0, 1, "interval must be positive");
	luaL_checktype(l, 2, LUA_TFUNCTION);
	its.it_value.tv_sec  = (time_t) secs;
	its.it_value.tv_nsec = (long) ((secs - (time_t) secs) * 1e9);
	if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
		its.it_value.tv_nsec = 1;
	t         = ecalloc(1, sizeof(Timer));
	t->repeat = lua_toboolean(l, 3);
	if (t->repeat)
		its.it_interval = its.it_value;
	if ((t->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC))
	        < 0
	    || timerfd_settime(t->fd, 0, &its, NULL) < 0) {
		if (t->fd >= 0)
			close(t->fd);
		free(t);
		return luaL_error(l, "timerfd: %s", strerror(errno));
	}
	lua_pushvalue(l, 2);
	t->ref   = luaL_ref(l, LUA_REGISTRYINDEX);
	t->owner = mainstate(l);
	t->id    = ++timerseq;
	t->next  = timers;
	timers   = t;
	if (started && t->owner == L)
		armtimer(t);
	lua_pushinteger(l, t->id);
	return 1;
}

/* ewm.cancel(id) */
static int l_cancel(lua_State *l) {
	lua_Integer id = luaL_checkinteger(l, 1);
	Timer *t;

	for (t = timers; t; t = t->next)
		if (t->id == id) {
			luaL_unref(t->owner, LUA_REGISTRYINDEX, t->ref);
			freetimer(t);
			break;
		}
	return 0;
}

static int l_clients(lua_State *l) {
	Monitor *m;
	Client *c;
	int n = 0;

	lua_newtable(l);
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next) {
			pushclient(l, c);
			lua_rawseti(l, -2, ++n);
		}
	return 1;
}

static int l_focused(lua_State *l) {
	pushclient(l, selmon ? selmon->sel : NULL);
	return 1;
}

static int l_monitor(lua_State *l) {
	needwm(l, "monitor");
	lua_newtable(l);
	setfield(l, "num", selmon->num);
	setfield(l, "x", selmon->wx);
	setfield(l, "y", selmon->wy);
	setfield(l, "w", selmon->ww);
	setfield(l, "h", selmon->wh);
	setfield(l, "nmaster", selmon->nmaster);
	lua_pushnumber(l, selmon->mfact);
	lua_setfield(l, -2, "mfact");
	pushtags(l, selmon->tagset[selmon->seltags]);
	lua_setfield(l, -2, "tags");
	lua_pushstring(l, selmon->lt[selmon->sellt]->name);
	lua_setfield(l, -2, "layout");
	return 1;
}

static int l_setstatus(lua_State *l) {
	setstatus(luaL_checkstring(l, 1));
	return 0;
}

static int l_client_focus(lua_State *l) {
	Client *c = checkclient(l, 1);
	Arg a     = {.ui = c->tags};

	if (!ISVISIBLE(c)) {
		selmon = c->mon;
		view(&a);
	}
	focus(c);
	restack(c->mon);
	return 0;
}

static int l_client_kill(lua_State *l) {
	closeclient(checkclient(l, 1));
	return 0;
}

static int l_client_settags(lua_State *l) {
	Client *c = checkclient(l, 1);

	settags(c, checktags(l, 2));
	return 0;
}

static int l_client_setfloating(lua_State *l) {
	Client *c = checkclient(l, 1);

	setfloating(c, lua_toboolean(l, 2));
	return 0;
}

static int l_client_setfullscreen(lua_State *l) {
	Client *c = checkclient(l, 1);

	setfullscreen(c, lua_toboolean(l, 2));
	return 0;
}

/* ewm.autostart(cmd): spawn cmd (like ewm.spawn) once the WM has started;
 * ignored when the configuration is reloaded later */
static int l_autostart(lua_State *l) {
	luaL_argcheck(l, lua_isstring(l, 1) || lua_istable(l, 1), 1,
	              "command string or argv table expected");
	if (started)
		return 0;
	lua_getfield(l, LUA_REGISTRYINDEX, AUTOSTART);
	lua_pushvalue(l, 1);
	lua_rawseti(l, -2, luaL_len(l, -2) + 1);
	return 0;
}

static int openewm(lua_State *l) {
	static const luaL_Reg funcs[] = {
	    {"set", l_set},           {"key", l_key},         {"button", l_button},
	    {"rule", l_rule},         {"layout", l_layout},   {"on", l_on},
	    {"timer", l_timer},       {"cancel", l_cancel},   {"clients", l_clients},
	    {"autostart", l_autostart},
	    {"focused", l_focused},   {"monitor", l_monitor}, {"setstatus", l_setstatus},
	    {NULL, NULL},
	};
	static const luaL_Reg client[] = {
	    {"focus", l_client_focus},
	    {"kill", l_client_kill},
	    {"settags", l_client_settags},
	    {"setfloating", l_client_setfloating},
	    {"setfullscreen", l_client_setfullscreen},
	    {NULL, NULL},
	};
	size_t i;

	luaL_newlib(l, funcs);
	for (i = 0; i < LENGTH(actions); i++) {
		lua_pushlightuserdata(l, (void *) &actions[i]);
		lua_pushcclosure(l, l_action, 1);
		lua_setfield(l, -2, actions[i].name);
	}
	luaL_newlib(l, client);
	lua_setfield(l, -2, "client");
	lua_pushstring(l, VERSION);
	lua_setfield(l, -2, "version");
	lua_pushstring(l, cfgdir);
	lua_setfield(l, -2, "configdir");
	return 1;
}

/* loading */

static int runfile(lua_State *l, const char *path) {
	if (luaL_loadfile(l, path) != LUA_OK) {
		report("%s", lua_tostring(l, -1));
		lua_pop(l, 1);
		return -1;
	}
	return pcall(l, 0, 0, path);
}

static int isluafile(const struct dirent *e) {
	size_t n = strlen(e->d_name);

	return e->d_name[0] != '.' && n > 4 && !strcmp(e->d_name + n - 4, ".lua");
}

/* run the .lua files in plugins/ in name order; a broken plugin is reported but does not
 * fail the configuration */
static void loadplugins(lua_State *l, const char *dir) {
	char path[PATH_MAX];
	struct dirent **list;
	int i, n;

	if (snprintf(path, sizeof(path), "%s/plugins", dir) >= (int) sizeof(path)
	    || (n = scandir(path, &list, isluafile, alphasort)) < 0)
		return;
	for (i = 0; i < n; i++) {
		if (snprintf(path, sizeof(path), "%s/plugins/%s", dir,
		             list[i]->d_name)
		    < (int) sizeof(path))
			runfile(l, path);
		free(list[i]);
	}
	free(list);
}

/* apply ewm.set{layouts = {...}}: the listed layouts come first, in that
 * order, followed by the unlisted ones (e.g. from plugins) */
static int orderlayouts(Config *c) {
	Layout tmp;
	size_t i, j;

	for (i = 0; i < nlayoutorder; i++) {
		for (j = i; j < c->nlayouts && strcmp(c->layouts[j].name, layoutorder[i]);
		     j++)
			;
		if (j == c->nlayouts) {
			report("layouts: unknown or repeated layout '%s'", layoutorder[i]);
			return -1;
		}
		tmp           = c->layouts[i];
		c->layouts[i] = c->layouts[j];
		c->layouts[j] = tmp;
	}
	return 0;
}

/* copy the installed default config.lua to dst, creating its directory */
static int installdefault(const char *dst) {
	char src[PATH_MAX], dir[PATH_MAX], buf[4096], *slash;
	FILE *in, *out = NULL;
	size_t n;
	int fd, err = 0;

	snprintf(src, sizeof(src), "%s/config.lua", DATADIR);
	if (!(in = fopen(src, "r")))
		return -1;
	snprintf(dir, sizeof(dir), "%s", dst);
	if ((slash = strrchr(dir, '/'))) {
		*slash = '\0';
		mkdirp(dir);
	}
	if ((fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0
	    || !(out = fdopen(fd, "w"))) {
		if (fd >= 0)
			close(fd);
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, n, out) != n)
			err = 1;
	err |= ferror(in) | (fclose(out) != 0);
	fclose(in);
	if (err) {
		unlink(dst);
		return -1;
	}
	fprintf(stderr, "ewm: created %s from %s\n", dst, src);
	return 0;
}

/* locate config.lua: $EWM_CONFIG, else ~/.config/ewm/config.lua, which is
 * created from the installed default at startup (install); the default
 * itself is only used if that fails */
static int findconfig(char *path, size_t len, int install) {
	const char *env = getenv("EWM_CONFIG");

	if (env && *env) {
		snprintf(path, len, "%s", env);
		return 0;
	}
	if (configpath(path, len, "config.lua") == 0
	    && (access(path, R_OK) == 0 || (install && installdefault(path) == 0)))
		return 0;
	snprintf(path, len, "%s/config.lua", DATADIR);
	return access(path, R_OK);
}

/* build *out and a Lua state from config.lua; 0 on success, 1 if there is
 * no config.lua, -1 on errors (reported) */
static int load(Config *out, lua_State **outl, int install) {
	char path[PATH_MAX], dir[PATH_MAX], *slash;
	lua_State *l;
	int res;

	if (findconfig(path, sizeof(path), install) != 0)
		return 1;
	snprintf(dir, sizeof(dir), "%s", path);
	if ((slash = strrchr(dir, '/')))
		*slash = '\0';
	else
		snprintf(dir, sizeof(dir), ".");

	if (!(l = luaL_newstate()))
		die("ewm: cannot create Lua state");
	luaL_openlibs(l);
	lua_newtable(l);
	lua_setfield(l, LUA_REGISTRYINDEX, HOOKS);
	lua_newtable(l);
	lua_setfield(l, LUA_REGISTRYINDEX, AUTOSTART);
	/* require() finds modules next to config.lua */
	lua_getglobal(l, "package");
	lua_pushfstring(l, "%s/?.lua;%s/?/init.lua;", dir, dir);
	lua_getfield(l, -2, "path");
	lua_concat(l, 2);
	lua_setfield(l, -2, "path");
	lua_pushfstring(l, "%s/?.so;", dir);
	lua_getfield(l, -2, "cpath");
	lua_concat(l, 2);
	lua_setfield(l, -2, "cpath");
	lua_pop(l, 1);

	setdefaults(out);
	loadL   = l;
	loadcfg = out;
	snprintf(cfgdir, sizeof(cfgdir), "%s", dir);
	luaL_requiref(l, "ewm", openewm, 1);
	lua_pop(l, 1);

	res = runfile(l, path);
	if (res == 0) {
		loadplugins(l, dir);
		res = orderlayouts(out);
	}
	freestrv(layoutorder, nlayoutorder);
	layoutorder  = NULL;
	nlayoutorder = 0;
	loadL        = NULL;
	loadcfg      = NULL;
	if (res < 0) {
		Timer *t, *next;

		for (t = timers; t; t = next) {
			next = t->next;
			if (t->owner == l)
				freetimer(t);
		}
		lua_close(l);
		config_free(out);
		return -1;
	}
	*outl = l;
	return 0;
}

/* inotify on the configuration directories */

static void watchfd(int fd, int add) {
	struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0};

	ev.data.fd = fd;
	epoll_ctl(epoll_fd, add ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, fd, &ev);
}

static void unwatch(void) {
	if (inotifyfd >= 0) {
		watchfd(inotifyfd, 0);
		close(inotifyfd);
		inotifyfd = -1;
	}
}

/* watch the directory of the active config.lua, its plugins/, and
 * ~/.config/ewm so creating a personal config.lua takes effect */
static void rewatch(void) {
	char dirs[3][PATH_MAX];
	size_t i;

	unwatch();
	if (!cfg.autoreload
	    || (inotifyfd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) < 0)
		return;
	snprintf(dirs[0], sizeof(dirs[0]), "%s", cfgdir);
	if (snprintf(dirs[1], sizeof(dirs[1]), "%s/plugins", cfgdir)
	    >= (int) sizeof(dirs[1]))
		dirs[1][0] = '\0';
	if (configpath(dirs[2], sizeof(dirs[2]), "") < 0)
		dirs[2][0] = '\0';
	for (i = 0; i < LENGTH(dirs); i++)
		if (dirs[i][0])
			inotify_add_watch(inotifyfd, dirs[i], WATCHMASK);
	watchfd(inotifyfd, 1);
}

/* a burst of editor writes becomes one reload */
static void debounce(void) {
	struct itimerspec its = {.it_value = {0, DEBOUNCE}};

	if (debouncefd < 0) {
		if ((debouncefd = timerfd_create(CLOCK_MONOTONIC,
		                                 TFD_NONBLOCK | TFD_CLOEXEC))
		    < 0)
			return;
		watchfd(debouncefd, 1);
	}
	timerfd_settime(debouncefd, 0, &its, NULL);
}

static void readinotify(void) {
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *ev;
	ssize_t n;
	char *p;
	size_t len;
	int relevant = 0;

	while ((n = read(inotifyfd, buf, sizeof(buf))) > 0)
		for (p = buf; p < buf + n; p += sizeof(*ev) + ev->len) {
			ev  = (const struct inotify_event *) p;
			len = ev->len ? strlen(ev->name) : 0;
			/* *.lua files, and plugins/ appearing or going away */
			if ((len > 4 && !strcmp(ev->name + len - 4, ".lua"))
			    || (len && !strcmp(ev->name, "plugins")))
				relevant = 1;
		}
	if (relevant)
		debounce();
}

static void runtimer(Timer *t) {
	uint64_t expirations;
	int id = t->id, ref = t->ref;
	Timer *p;

	if (read(t->fd, &expirations, sizeof(expirations)) < 0)
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	pcall(L, 0, 0, "timer");
	/* one-shot timers go away, unless the callback cancelled already */
	for (p = timers; p && p->id != id; p = p->next)
		;
	if (p && !p->repeat) {
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
		freetimer(p);
	}
}

int config_handlefd(int fd) {
	uint64_t expirations;
	Timer *t;

	if (fd == inotifyfd && fd >= 0) {
		readinotify();
		return 1;
	}
	if (fd == debouncefd && fd >= 0) {
		if (read(debouncefd, &expirations, sizeof(expirations)) > 0)
			config_reload();
		return 1;
	}
	for (t = timers; t; t = t->next)
		if (t->fd == fd) {
			runtimer(t);
			return 1;
		}
	return 0;
}

/* entry points */

static void droptimers(lua_State *owner) {
	Timer *t, *next;

	for (t = timers; t; t = next) {
		next = t->next;
		if (t->owner == owner)
			freetimer(t);
	}
}

void config_init(void) {
	int res = load(&cfg, &L, 1);

	if (res == 0)
		return;
	if (res > 0) {
		fprintf(stderr, "ewm: no config.lua found, using built-in defaults\n");
		/* watch the personal configuration directory for a new one */
		if (configpath(cfgdir, sizeof(cfgdir), "") < 0)
			cfgdir[0] = '\0';
		else if (cfgdir[strlen(cfgdir) - 1] == '/')
			cfgdir[strlen(cfgdir) - 1] = '\0';
	} else {
		report("%s, using built-in defaults", errbuf + strlen("ewm: "));
	}
	setdefaults(&cfg);
	setfallbackbindings(&cfg);
	L = NULL;
}

void config_fallback(const char *reason) {
	report("%s, using built-in defaults", reason);
	droptimers(L);
	if (L)
		lua_close(L);
	L = NULL;
	config_free(&cfg);
	setdefaults(&cfg);
	setfallbackbindings(&cfg);
}

void config_start(void) {
	Timer *t;
	int i, n;

	started = 1;
	for (t = timers; t; t = t->next)
		if (t->owner == L)
			armtimer(t);
	rewatch();
	if (errbuf[0])
		setstatus(errbuf);
	if (L) {
		/* run ewm.autostart commands through ewm.spawn */
		lua_getfield(L, LUA_REGISTRYINDEX, AUTOSTART);
		n = luaL_len(L, -1);
		for (i = 1; i <= n; i++) {
			lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
			lua_getfield(L, -1, "ewm");
			lua_getfield(L, -1, "spawn");
			lua_rawgeti(L, -4, i);
			pcall(L, 1, 0, "autostart");
			lua_pop(L, 2);
		}
		lua_pop(L, 1);
	}
	config_hook("startup", NULL);
}

void config_reload(void) {
	Config newcfg, old;
	lua_State *nl, *oldl = L;
	const char *err;
	Timer *t;
	int res;

	errbuf[0] = '\0';
	if ((res = load(&newcfg, &nl, 0)) != 0) {
		if (res > 0)
			report("no config.lua found");
		return;
	}
	old = cfg;
	cfg = newcfg;
	L   = nl;
	if ((err = applyconfig(&old))) {
		/* nothing was changed yet, return to the old configuration */
		cfg = old;
		L   = oldl;
		droptimers(nl);
		lua_close(nl);
		config_free(&newcfg);
		report("%s", err);
		return;
	}
	droptimers(oldl);
	if (oldl)
		lua_close(oldl);
	config_free(&old);
	for (t = timers; t; t = t->next)
		if (t->owner == L)
			armtimer(t);
	rewatch();
	fprintf(stderr, "ewm: configuration reloaded\n");
}

void config_cleanup(void) {
	unwatch();
	if (debouncefd >= 0)
		close(debouncefd);
	while (timers)
		freetimer(timers);
	if (L)
		lua_close(L);
	L = NULL;
	config_free(&cfg);
}

/* callbacks from ewm.c */

/* push the bound function and its arguments; returns the argument count */
static int pushbinding(int ref) {
	int i, n;

	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	n = luaL_len(L, -1);
	for (i = 1; i <= n; i++)
		lua_rawgeti(L, -i, i);
	lua_remove(L, -n - 1);
	return n - 1;
}

void config_call(const Arg *arg) {
	if (L)
		pcall(L, pushbinding(arg->i), 0, "key binding");
}

void config_callbutton(const Arg *arg, int tag) {
	int nargs;

	if (!L)
		return;
	/* tag bar buttons without arguments get the clicked tag */
	if ((nargs = pushbinding(arg->i)) == 0 && tag > 0) {
		lua_pushinteger(L, tag);
		nargs = 1;
	}
	pcall(L, nargs, 0, "button binding");
}

void config_hook(const char *event, Client *c) {
	int i, n;

	if (!L || hookdepth)
		return;
	lua_getfield(L, LUA_REGISTRYINDEX, HOOKS);
	if (lua_getfield(L, -1, event) != LUA_TTABLE) {
		lua_pop(L, 2);
		return;
	}
	hookdepth++; /* hooks acting on clients must not recurse into hooks */
	n = luaL_len(L, -1);
	for (i = 1; i <= n; i++) {
		lua_rawgeti(L, -1, i);
		if (c)
			pushclient(L, c);
		pcall(L, c ? 1 : 0, 0, event);
	}
	hookdepth--;
	lua_pop(L, 2);
}

void config_layout(Monitor *m) {
	const Layout *lt = m->lt[m->sellt];
	Client *c;
	int i, n, x, y, w, h;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++)
		;
	if (n == 0 || !L)
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, lt->ref);
	lua_newtable(L);
	setfield(L, "x", m->wx);
	setfield(L, "y", m->wy);
	setfield(L, "w", m->ww);
	setfield(L, "h", m->wh);
	setfield(L, "gap", m->gappx);
	setfield(L, "nmaster", m->nmaster);
	setfield(L, "monitor", m->num);
	lua_pushnumber(L, m->mfact);
	lua_setfield(L, -2, "mfact");
	lua_pushinteger(L, n);
	if (pcall(L, 2, 1, lt->name) < 0)
		return;
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		report("layout %s: must return a table of boxes", lt->name);
		return;
	}
	for (i = 1, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++) {
		if (lua_rawgeti(L, -1, i) != LUA_TTABLE) {
			lua_pop(L, 1);
			continue;
		}
		lua_getfield(L, -1, "x");
		lua_getfield(L, -2, "y");
		lua_getfield(L, -3, "w");
		lua_getfield(L, -4, "h");
		x = lua_tointeger(L, -4);
		y = lua_tointeger(L, -3);
		w = lua_tointeger(L, -2);
		h = lua_tointeger(L, -1);
		lua_pop(L, 5);
		resize(c, x, y, w - 2 * c->bw, h - 2 * c->bw, 0);
	}
	lua_pop(L, 1);
}
