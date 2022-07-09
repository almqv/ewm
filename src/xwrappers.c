#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>

#include "xwrappers.h"

static Atom atoms[LastAtom];
static int atoms_intialised = 0;

Atom *
get_atoms(Display *dpy)
{
	if (!atoms_intialised) {
		atoms[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
		atoms[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
		atoms[WMState] = XInternAtom(dpy, "WM_STATE", False);
		atoms[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
		atoms[WMChangeState] = XInternAtom(dpy, "WM_CHANGE_STATE", False);
		atoms[WMWindowRole] = XInternAtom(dpy, "WM_WINDOW_ROLE", False);

		atoms[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
		atoms[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
		atoms[NetSystemTray] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
		atoms[NetSystemTrayOP] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
		atoms[NetSystemTrayOrientation] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
		atoms[NetSystemTrayOrientationHorz] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION_HORZ", False);
		atoms[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
		atoms[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
		atoms[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
		atoms[NetWMFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
		atoms[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
		atoms[NetWMWindowTypeDock] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
		atoms[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
		atoms[NetWMWindowTypeDesktop] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
		atoms[NetWMMaximizedVert] = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
		atoms[NetWMMaximizedHorz] = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
		atoms[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
		atoms[NetClientListStacking] = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
		atoms[NetCurrentDesktop] = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
		atoms[NetDesktopNames] = XInternAtom(dpy, "_NET_DESKTOP_NAMES", False);
		atoms[NetDesktopViewport] = XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT", False);
		atoms[NetNumberOfDesktops] = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
		atoms[NetWMActionClose] = XInternAtom(dpy, "_NET_WM_ACTION_CLOSE", False);
		atoms[NetWMDemandsAttention] = XInternAtom(dpy, "_NET_WM_DEMANDS_ATTENTION", False);
		atoms[NetWMDesktop] = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
		atoms[NetWMMoveResize] = XInternAtom(dpy, "_NET_WM_MOVE_RESIZE", False);

		atoms[Manager] = XInternAtom(dpy, "MANAGER", False);
		atoms[Xembed] = XInternAtom(dpy, "_XEMBED", False);
		atoms[XembedInfo] = XInternAtom(dpy, "_XEMBED_INFO", False);

		atoms[WMClientTags] = XInternAtom(dpy, "_MWM_CLIENT_TAGS", False);
		atoms[WMCurrentTags] = XInternAtom(dpy, "_MWM_CURRENT_TAGS", False);
		atoms[WMClientMonitor] = XInternAtom(dpy, "_MWM_CLIENT_MONITOR", False);
		atoms[WMBorderWidth] = XInternAtom(dpy, "_MWM_BORDER_WIDTH", False);
		atoms[SteamGame] = XInternAtom(dpy, "STEAM_GAME", False);

		atoms[KDENetWMWindowTypeOverride] = XInternAtom(dpy, "_KDE_NET_WM_WINDOW_TYPE_OVERRIDE", False);
		atoms[Utf8] = XInternAtom(dpy, "UTF8_STRING", False);
		atoms[Motif] = XInternAtom(dpy, "_MOTIF_NetLast, WM_HINTS", False);

		atoms_intialised = 1;
	}
	return atoms;
}

// Function to set a windows state with x
void
window_set_state(Display *dpy, Window win, long state) {
	long data[] = {state, None};
	get_atoms(dpy);	

	XChangeProperty(dpy, win, atoms[WMState], atoms[WMState], 32, PropModeReplace, (unsigned char*)data, 2);
}

void 
window_map(Display *dpy, Window win, int deicon) {
	if (!win) // if the window does not exist then just panic and escape
		return;

	XMapWindow(dpy, win);
	if (deicon)
		window_set_state(dpy, win, NormalState);
	XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
}

void
window_umap(Display * dpy, Window win, Window root, int icon) {
	static XWindowAttributes ra, ca;
	if (!win)
		return;

	XGrabServer(dpy);

	XGetWindowAttributes(dpy, root, &ra); 
	XGetWindowAttributes(dpy, win, &ca); 

	XSelectInput(dpy, root, ra.your_event_mask & ~SubstructureNotifyMask);
	XSelectInput(dpy, win, ca.your_event_mask & ~SubstructureNotifyMask);

	XUnmapWindow(dpy, win);
	if (icon)
		window_set_state(dpy, win, IconicState);

	XSelectInput(dpy, root, ra.your_event_mask);
	XSelectInput(dpy, win, ca.your_event_mask);
	XUngrabServer(dpy);
}
