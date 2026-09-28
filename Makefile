# ewm - Epsilons Window Manager
# See LICENSE file for copyright and license details.

VERSION = 2.0

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
MANDIR     ?= $(DATADIR)/man
PKG_CONFIG ?= pkg-config
# pkg-config name of Lua 5.4: lua5.4 (Debian, Nix), lua (Arch), lua54 (Fedora)
LUA        ?= lua5.4
# set to 0 to build without multi-monitor support
XINERAMA   ?= 1

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
	install -Dm644 config.lua $(DESTDIR)$(DATADIR)/ewm/config.lua
	install -Dm644 ewm.desktop $(DESTDIR)$(DATADIR)/xsessions/ewm.desktop
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	sed "s/VERSION/$(VERSION)/g" ewm.1 > $(DESTDIR)$(MANDIR)/man1/ewm.1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/ewm.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ewm $(DESTDIR)$(BINDIR)/ewm-msg \
	      $(DESTDIR)$(DATADIR)/ewm/config.lua \
	      $(DESTDIR)$(DATADIR)/xsessions/ewm.desktop \
	      $(DESTDIR)$(MANDIR)/man1/ewm.1
	-rmdir $(DESTDIR)$(DATADIR)/ewm

clean:
	rm -rf $(BUILD)

.PHONY: all install uninstall clean
