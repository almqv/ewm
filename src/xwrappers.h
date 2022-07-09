#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>

enum { NetSupported, NetWMDemandsAttention, NetWMName, NetWMState, NetWMCheck,
		NetWMActionClose, NetWMActionMinimize, NetWMAction, NetWMMoveResize,
	   	NetWMMaximizedVert, NetWMMaximizedHorz,
	   	NetSystemTray, NetSystemTrayOP, NetSystemTrayOrientation, NetSystemTrayOrientationHorz,
	   	NetWMFullscreen, NetActiveWindow, NetWMWindowType, NetWMWindowTypeDock, NetWMDesktop,
	   	NetWMWindowTypeDesktop, NetWMWindowTypeDialog, NetClientList, NetClientListStacking,
	   	NetDesktopNames, NetDesktopViewport, NetNumberOfDesktops,
	   	NetCurrentDesktop, /* EWMH atoms */

		Manager, Xembed, XembedInfo,  /* Xembed atoms */

		SteamGame, WMClientTags, WMCurrentTags, WMClientMonitor, WMBorderWidth, WMLast, 

		WMProtocols, WMDelete, WMState, WMTakeFocus, WMChangeState,
		WMWindowRole, /* default atoms */

		KDENetWMWindowTypeOverride, Utf8, Motif, NetLast, LastAtom };

void window_set_state(Display*, Window, long);
void window_map(Display*, Window, int);
void window_umap(Display*, Window, Window, int);
