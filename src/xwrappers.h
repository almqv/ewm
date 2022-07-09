#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>

void window_set_state(Display*, Window, long);
void window_map(Display*, Window win, long state);
void window_umap(Display*, Window win, Window root, int);
