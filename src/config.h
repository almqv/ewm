/* See LICENSE file for copyright and license details. */
#include <X11/XF86keysym.h>

#define STATUSBAR "dwmblocks"
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* appearance */
static const unsigned int borderpx	= 4;		/* border pixel of windows */
static const unsigned int snap		= 16;		/* snap pixel (32)*/
static const unsigned int gappx		= 40;
static const unsigned int gapmodes[] 	= { gappx, 0 };
static const int showbar		= 1;		/* 0 means no bar */
static const int topbar			= 1;		/* 0 means bottom bar */
static const int usealtbar		= 0;		/* 1 means use non-dwm status bar */

static const char *altbarclass		= "Polybar"; /* Alternate bar class name */
static const char *alttrayname		= "tray";	 /* Polybar tray instance name */
static const char *altbarcmd		= ""; /*"$HOME/.config/polybar/launch";*/ /* Alternate bar launch command */
static const char *fonts[]		= { "Source Code Pro:size=10" };
static const char dmenufont[]		= "Source Code Pro:size=12";

static const char bg_color[]		= "#282c34";
static const char bg_color_bar[]	= "#282c34";
static const char inactive_bg_color[]	= "#272727";
static const char text_color[]		= "#d8dee9";
static const char inactive_text_color[]	= "#676E7D";
static const char urgent_bg_color[]	= "#E53935";
static const char indicator_color[]	= "#474747";
static const char selected_color[]	= "#81a2be";

static const char col_gray1[]		= "#222222";
static const char col_gray2[]		= "#444444";
static const char col_gray3[]		= "#bbbbbb";
static const char col_gray4[]		= "#eeeeee";
static const char col_cyan[]		= "#5f819d";
static const char *colors[][3]		= {
	/*		fg		bg		border	*/
	[SchemeNorm] = { col_gray3, bg_color_bar, col_gray1 },
	[SchemeSel]  = { selected_color, bg_color_bar, col_gray2 },
};

/* tagging */
static const char *tags[] = { "o", "o", "o", "o", "o", "o", "o", "o", "o" };

static const Rule rules[] = {
	/* xprop(1):
	 *	WM_CLASS(STRING) = instance, class
	 *	WM_NAME(STRING) = title
	 */
	/* class	  instance	  title		  tags mask		isfloating	 monitor */
	{ "Gimp",	  NULL,		  NULL,		  0,			1,			 -1 },
	{ "Firefox",	  NULL,		  NULL,		  1 << 8,		0,			 -1 },
	{ "discord",	  NULL,		  NULL,		  0,			0,			 -1 },
	{ "Steam",	  NULL,		  NULL,		  0,			0,			 -1 },
	{ "Qemu-system-x86_64",	  NULL,	  NULL,		  0,			1,			 -1 },
};

/* layout(s) */
static const float mfact	 = 0.55; /* factor of master area size [0.05..0.95] */
static const int nmaster	 = 1;	 /* number of clients in master area */
static const int resizehints = 1;	 /* 1 means respect size hints in tiled resizals */

static const Layout layouts[] = {
	/* symbol	  arrange function */
	{ "[]=",	  tile },	 /* first entry is default */
	{ "><>",	  NULL },	 
	{ "[M]",	  monocle },
};

/* key definitions */
#define MODKEY Mod4Mask
#define TAGKEYS(KEY,TAG) \
	{ MODKEY,						KEY,	  view,			  {.ui = 1 << TAG} }, \
	{ MODKEY|ControlMask,			KEY,	  toggleview,	  {.ui = 1 << TAG} }, \
	{ MODKEY|ShiftMask,				KEY,	  tag,			  {.ui = 1 << TAG} }, \
	{ MODKEY|ControlMask|ShiftMask, KEY,	  toggletag,	  {.ui = 1 << TAG} },

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands */
static char dmenumon[2] = "0"; /* component of dmenucmd, manipulated in spawn() */
static const char *dmenucmd[] = { "dmenu_run", "-m", dmenumon, "-p", "$>", "-z", "512", NULL };
static const char *termcmd[]  = { "alacritty", NULL };
static const char *betterlockscreencmd[] = { "betterlockscreen", "--lock", "blur", NULL };
static const char *screenshotcmd[] = { "flameshot", "gui", NULL };
static const char *cmuspausecmd[] = { "cmus-remote", "--pause", NULL };

static Key keys[] = {
	/* modifier						key			function			argument */
	{ MODKEY,		 				XF86XK_AudioLowerVolume,  spawn, SHCMD("amixer -q set Master 5%- unmute; pkill -RTMIN+1 dwmblocks") },
	{ MODKEY,		 				XF86XK_AudioRaiseVolume,  spawn, SHCMD("amixer -q set Master 5%+ unmute; pkill -RTMIN+1 dwmblocks") },
	{ MODKEY,						XF86XK_AudioMute, spawn, SHCMD("amixer -q set Master toggle; pkill -RTMIN+1 dwmblocks") },
	{ MODKEY|ShiftMask,					XF86XK_AudioMute, spawn, SHCMD("amixer set Capture toggle") },
	{ MODKEY,						XK_d,		spawn,			{.v = dmenucmd } },
	{ MODKEY,						XK_Return, 	spawn,			{.v = termcmd } },
	{ MODKEY,						XK_b,		togglebar,		{0} },
	{ MODKEY,						XK_j,		focusstack,		{.i = +1 } },
	{ MODKEY,						XK_k,		focusstack,		{.i = -1 } },
	{ MODKEY,						XK_i,		incnmaster,		{.i = +1 } },
	{ MODKEY,						XK_p,		incnmaster,		{.i = -1 } },
	{ MODKEY,						XK_h,		setmfact,		{.f = -0.05} },
	{ MODKEY,						XK_l,		setmfact,		{.f = +0.05} },
	{ MODKEY|ShiftMask,					XK_Return, zoom,			{0} },
	{ MODKEY,						XK_Tab,		view,			{0} },
	{ MODKEY|ShiftMask,					XK_q,		killclient,		{0} },
	{ MODKEY,						XK_Down,	moveresize,		{.v = "0x 25y 0w 0h" } },
	{ MODKEY,						XK_Up,		moveresize,		{.v = "0x -25y 0w 0h" } },
	{ MODKEY,						XK_Right,  moveresize,		{.v = "25x 0y 0w 0h" } },
	{ MODKEY,						XK_Left,	moveresize,		{.v = "-25x 0y 0w 0h" } },
	{ MODKEY|ShiftMask,					XK_Down,	moveresize,		{.v = "0x 0y 0w 25h" } },
	{ MODKEY|ShiftMask,					XK_Up,		moveresize,		{.v = "0x 0y 0w -25h" } },
	{ MODKEY|ShiftMask,					XK_Right,  moveresize,		{.v = "0x 0y 25w 0h" } },
	{ MODKEY|ShiftMask,					XK_Left,	moveresize,		{.v = "0x 0y -25w 0h" } },
	{ MODKEY|ControlMask,					XK_Up,		moveresizeedge, {.v = "t"} },
	{ MODKEY|ControlMask,					XK_Down,	moveresizeedge, {.v = "b"} },
	{ MODKEY|ControlMask,					XK_Left,	moveresizeedge, {.v = "l"} },
	{ MODKEY|ControlMask,					XK_Right,  moveresizeedge, {.v = "r"} },
	{ MODKEY|ControlMask|ShiftMask,				XK_Up,		moveresizeedge, {.v = "T"} },
	{ MODKEY|ControlMask|ShiftMask, 			XK_Down,	moveresizeedge, {.v = "B"} },
	{ MODKEY|ControlMask|ShiftMask, 			XK_Left,	moveresizeedge, {.v = "L"} },
	{ MODKEY|ControlMask|ShiftMask, 			XK_Right,  moveresizeedge, {.v = "R"} },
	{ MODKEY,						XK_t,		setlayout,		{.v = &layouts[0]} },
	{ MODKEY,						XK_f,		setlayout,		{.v = &layouts[1]} },
	{ MODKEY,						XK_m,		setlayout,		{.v = &layouts[2]} },
	{ MODKEY,						XK_space,  setlayout,		{0} },
	{ MODKEY|ShiftMask,					XK_space,  togglefloating, {0} },
	{ MODKEY|ShiftMask,					XK_f,		togglefullscr,  {0} },
	{ MODKEY,						XK_0,		view,			{.ui = ~0 } },
	{ MODKEY|ShiftMask,					XK_0,		tag,				{.ui = ~0 } },
	{ MODKEY,						XK_comma,  focusmon,		{.i = -1 } },
	{ MODKEY,						XK_period, focusmon,		{.i = +1 } },
	{ MODKEY|ShiftMask,					XK_comma,  tagmon,			{.i = -1 } },
	{ MODKEY|ShiftMask,					XK_period, tagmon,			{.i = +1 } },
	{ MODKEY|ShiftMask,					XK_x,		spawn,			{.v = betterlockscreencmd} },
	{ MODKEY,						XK_Print,  spawn,			{.v = screenshotcmd} },
	{ MODKEY,						XK_u,		spawn,			{.v = cmuspausecmd} },
	{ MODKEY,						XK_g,		switchgaps,		{.i = +1 } },
	{ MODKEY,						XK_v,		switchgaps,		{.i = -1 } },
	{ MODKEY,						XK_minus,  setgaps,			{.i = -1 } },
	{ MODKEY,						XK_plus,	setgaps,			{.i = +1 } },
	{ MODKEY|ShiftMask,					XK_equal,  setgaps,			{.i = 0	} },
	TAGKEYS(						XK_1,						0)
	TAGKEYS(						XK_2,						1)
	TAGKEYS(						XK_3,						2)
	TAGKEYS(						XK_4,						3)
	TAGKEYS(						XK_5,						4)
	TAGKEYS(						XK_6,						5)
	TAGKEYS(						XK_7,						6)
	TAGKEYS(						XK_8,						7)
	TAGKEYS(						XK_9,						8)
	{ MODKEY|ShiftMask,					XK_e,		quit,			{0} },
};

/* button definitions */
/* click can be ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle, ClkClientWin, or ClkRootWin */
static Button buttons[] = {
	/* click				event mask		button			function		argument */
	{ ClkStatusText,		0,				Button1,		sigstatusbar,		{.i = 1} },
	{ ClkStatusText,		0,				Button2,		sigstatusbar,		{.i = 2} },
	{ ClkStatusText,		0,				Button3,		sigstatusbar,		{.i = 3} },
	{ ClkStatusText,		0,				Button4,		sigstatusbar,		{.i = 4} },
	{ ClkStatusText,		0,				Button5,		sigstatusbar,		{.i = 5} },
	{ ClkLtSymbol,			0,				Button1,		setlayout,		{0} },
	{ ClkLtSymbol,			0,				Button3,		setlayout,		{.v = &layouts[2]} },
	{ ClkWinTitle,			0,				Button2,		zoom,			{0} },
	{ ClkStatusText,		0,				Button2,		spawn,			{.v = termcmd } },
	{ ClkClientWin,			MODKEY,				Button1,		movemouse,		{0} },
	{ ClkClientWin,			MODKEY,				Button2,		togglefloating,		{0} },
	{ ClkClientWin,			MODKEY,				Button3,		resizemouse,		{0} },
	{ ClkTagBar,			0,				Button1,		view,			{0} },
	{ ClkTagBar,			0,				Button3,		toggleview,		{0} },
	{ ClkTagBar,			MODKEY,				Button1,		tag,			{0} },
	{ ClkTagBar,			MODKEY,				Button3,		toggletag,		{0} },
};


static const char *ipcsockpath = "/tmp/dwm.sock";
static IPCCommand ipccommands[] = {
  IPCCOMMAND(  view,				1,		{ARG_TYPE_UINT}	),
  IPCCOMMAND(  toggleview,			1,		{ARG_TYPE_UINT}	),
  IPCCOMMAND(  tag,					1,		{ARG_TYPE_UINT}	),
  IPCCOMMAND(  toggletag,			1,		{ARG_TYPE_UINT}	),
  IPCCOMMAND(  tagmon,				1,		{ARG_TYPE_UINT}	),
  IPCCOMMAND(  focusmon,			1,		{ARG_TYPE_SINT}	),
  IPCCOMMAND(  focusstack,			1,		{ARG_TYPE_SINT}	),
  IPCCOMMAND(  zoom,				1,		{ARG_TYPE_NONE}	),
  IPCCOMMAND(  incnmaster,			1,		{ARG_TYPE_SINT}	),
  IPCCOMMAND(  killclient,			1,		{ARG_TYPE_SINT}	),
  IPCCOMMAND(  togglefloating,		1,		{ARG_TYPE_NONE}	),
  IPCCOMMAND(  setmfact,			1,		{ARG_TYPE_FLOAT}  ),
  IPCCOMMAND(  setlayoutsafe,		1,		{ARG_TYPE_PTR}	  ),
  IPCCOMMAND(  quit,				1,		{ARG_TYPE_NONE}	)
};
