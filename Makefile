# ewm - Epsilons Window Manager
# See LICENSE file for copyright and license details.

VERSION = 2.0

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
DATADIR     ?= $(PREFIX)/share
MANDIR      ?= $(DATADIR)/man
# display managers look for sessions here; install.sh uses /usr/share/xsessions
XSESSIONDIR ?= $(DATADIR)/xsessions
PKG_CONFIG  ?= pkg-config
# set to 0 to build without multi-monitor support
XINERAMA    ?= 1

# Lua >= 5.3; its pkg-config name differs between distributions
LUA ?= $(firstword $(foreach p,lua5.4 lua-5.4 lua54 lua5.3 lua-5.3 lua53 lua, \
         $(shell $(PKG_CONFIG) --atleast-version=5.3 $(p) 2>/dev/null && echo $(p))))
LUA := $(LUA)
ifeq ($(filter clean uninstall,$(MAKECMDGOALS)),)
ifeq ($(LUA),)
$(error Lua >= 5.3 development files not found; set LUA=<pkg-config name>)
endif
endif

PKGS = x11 xft fontconfig yajl $(LUA)
DEFS = -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
       -DVERSION=\"$(VERSION)\" -DDATADIR=\"$(DATADIR)/ewm\"
ifeq ($(XINERAMA),1)
PKGS += xinerama
DEFS += -DXINERAMA
endif

# CFLAGS, CPPFLAGS and LDFLAGS are left to the user (e.g. make CFLAGS=-g)
CFLAGS   ?= -O2
EWM_CFLAGS := -std=c99 -pedantic -Wall -Wextra -Wno-unused-parameter \
              -Wno-sign-compare \
              $(DEFS) $(shell $(PKG_CONFIG) --cflags $(PKGS))
EWM_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS))
MSG_LIBS   := $(shell $(PKG_CONFIG) --libs yajl)

BUILD   = build
EWM_OBJ = $(addprefix $(BUILD)/,ewm.o config.o drw.o ipc.o IPCClient.o \
                                util.o yajl_dumps.o)
MSG_OBJ = $(BUILD)/ewm-msg.o

all: $(BUILD)/ewm $(BUILD)/ewm-msg

$(BUILD):
	mkdir -p $@

# -MMD keeps header dependencies in build/*.d
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(EWM_CFLAGS) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/ewm: $(EWM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(EWM_OBJ) $(EWM_LIBS)

$(BUILD)/ewm-msg: $(MSG_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(MSG_OBJ) $(MSG_LIBS)

-include $(EWM_OBJ:.o=.d) $(MSG_OBJ:.o=.d)

install: all
	install -Dm755 $(BUILD)/ewm $(DESTDIR)$(BINDIR)/ewm
	install -Dm755 $(BUILD)/ewm-msg $(DESTDIR)$(BINDIR)/ewm-msg
	install -Dm644 defaults/config.lua $(DESTDIR)$(DATADIR)/ewm/config.lua
	install -Dm644 -t $(DESTDIR)$(DATADIR)/ewm/examples examples/plugins/*.lua
	mkdir -p $(DESTDIR)$(XSESSIONDIR) $(DESTDIR)$(MANDIR)/man1
	sed "s|^Exec=.*|Exec=$(BINDIR)/ewm|" data/ewm.desktop \
		> $(DESTDIR)$(XSESSIONDIR)/ewm.desktop
	sed "s/VERSION/$(VERSION)/g" data/ewm.1 > $(DESTDIR)$(MANDIR)/man1/ewm.1
	chmod 644 $(DESTDIR)$(XSESSIONDIR)/ewm.desktop $(DESTDIR)$(MANDIR)/man1/ewm.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ewm $(DESTDIR)$(BINDIR)/ewm-msg \
	      $(DESTDIR)$(XSESSIONDIR)/ewm.desktop \
	      $(DESTDIR)$(MANDIR)/man1/ewm.1
	rm -rf $(DESTDIR)$(DATADIR)/ewm

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
