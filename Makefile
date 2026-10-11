VERSION   ?= 0.1.0
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
DATADIR   ?= $(PREFIX)/share
CC        ?= cc
CFLAGS    ?= -O2 -g
PROJECT_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DVERSION='"$(VERSION)"'

PKG_CONFIG ?= pkg-config
WL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl)
WL_LIBS    := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl)
# Override these if your distro lacks egl.pc / glesv2.pc
EGL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags egl glesv2 2>/dev/null)
EGL_LIBS   ?= $(shell $(PKG_CONFIG) --libs egl glesv2 2>/dev/null || echo -lEGL -lGLESv2)

WAYLAND_SCANNER ?= wayland-scanner
BUILD := build
PROTO := $(BUILD)/wlr-layer-shell-unstable-v1-client-protocol.h $(BUILD)/wlr-layer-shell-unstable-v1-protocol.c \
         $(BUILD)/xdg-shell-client-protocol.h $(BUILD)/xdg-shell-protocol.c \
         $(BUILD)/viewporter-client-protocol.h $(BUILD)/viewporter-protocol.c
SRC   := $(wildcard src/*.c)
PROTO_OBJ := $(BUILD)/wlr-layer-shell-unstable-v1-protocol.o $(BUILD)/xdg-shell-protocol.o $(BUILD)/viewporter-protocol.o
OBJ   := $(SRC:src/%.c=$(BUILD)/%.o) $(PROTO_OBJ)
INC   := -I$(BUILD) -Isrc -isystem third_party

all: driftpaper

driftpaper: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(WL_LIBS) $(EGL_LIBS) -lm

$(BUILD)/%.o: src/%.c $(PROTO) src/*.h | $(BUILD)
	$(CC) $(PROJECT_CFLAGS) $(CFLAGS) $(INC) $(WL_CFLAGS) $(EGL_CFLAGS) -c -o $@ $<

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c $(PROTO)
	$(CC) $(PROJECT_CFLAGS) $(CFLAGS) $(INC) $(WL_CFLAGS) -c -o $@ $<

$(BUILD)/%-client-protocol.h: protocol/%.xml | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(BUILD)/%-protocol.c: protocol/%.xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD):
	mkdir -p $@

install: driftpaper
	install -Dm755 driftpaper $(DESTDIR)$(BINDIR)/driftpaper
	sed 's|@BINDIR@|$(BINDIR)|' dist/driftpaper.service.in > $(BUILD)/driftpaper.service
	install -Dm644 $(BUILD)/driftpaper.service $(DESTDIR)$(PREFIX)/lib/systemd/user/driftpaper.service
	install -Dm644 examples/config.ini $(DESTDIR)$(DATADIR)/driftpaper/config.ini
	install -Dm644 LICENSE $(DESTDIR)$(DATADIR)/licenses/driftpaper/LICENSE

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/driftpaper $(DESTDIR)$(PREFIX)/lib/systemd/user/driftpaper.service
	rm -rf $(DESTDIR)$(DATADIR)/driftpaper $(DESTDIR)$(DATADIR)/licenses/driftpaper

clean:
	rm -rf $(BUILD) driftpaper

.PHONY: all install uninstall clean
.SECONDARY: $(PROTO)
