VERSION   ?= 1.1.1
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
DATADIR   ?= $(PREFIX)/share
APP_ID    := io.github.casper_rs.driftpaper
CC        ?= cc
CFLAGS    ?= -O2 -g
PROJECT_CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DVERSION='"$(VERSION)"'

PKG_CONFIG ?= pkg-config
WL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-egl)
WL_LIBS    := $(shell $(PKG_CONFIG) --libs wayland-client wayland-egl)
# Override these if your distro lacks egl.pc / glesv2.pc
EGL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags egl glesv2 2>/dev/null)
EGL_LIBS   ?= $(shell $(PKG_CONFIG) --libs egl glesv2 2>/dev/null || echo -lEGL -lGLESv2)
HAVE_GUI   := $(shell $(PKG_CONFIG) --exists gtk4 libadwaita-1 && echo yes)
GUI_CFLAGS := $(if $(HAVE_GUI),$(shell $(PKG_CONFIG) --cflags gtk4 libadwaita-1))
GUI_LIBS   := $(if $(HAVE_GUI),$(shell $(PKG_CONFIG) --libs gtk4 libadwaita-1))

WAYLAND_SCANNER ?= wayland-scanner
BUILD := build
PROTOCOLS := wlr-layer-shell-unstable-v1 xdg-shell viewporter fractional-scale-v1
PROTO_HEADERS := $(PROTOCOLS:%=$(BUILD)/%-client-protocol.h)
PROTO_OBJ := $(PROTOCOLS:%=$(BUILD)/%-protocol.o)
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(wildcard src/*.c)) $(PROTO_OBJ)
GUI_OBJ := $(patsubst gui/%.c,$(BUILD)/gui/%.o,$(wildcard gui/*.c)) \
           $(BUILD)/config.o $(BUILD)/analyze.o $(BUILD)/lockscreen.o $(BUILD)/runtime.o $(BUILD)/stb.o
INC := -I$(BUILD) -Isrc -isystem third_party
TARGETS := driftpaper $(if $(HAVE_GUI),driftpaper-settings)

all: $(TARGETS)
ifneq ($(HAVE_GUI),yes)
	@echo "Note: gtk4/libadwaita-1 development files not found, skipping driftpaper-settings"
endif

driftpaper: $(OBJ)
	$(CC) $(LDFLAGS) -pthread -o $@ $^ $(WL_LIBS) $(EGL_LIBS) -lm -ldl

driftpaper-settings: $(GUI_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(GUI_LIBS) -lm

$(BUILD)/%.o: src/%.c $(PROTO_HEADERS) src/*.h | $(BUILD)
	$(CC) $(PROJECT_CFLAGS) $(CFLAGS) $(INC) $(WL_CFLAGS) $(EGL_CFLAGS) -c -o $@ $<

$(BUILD)/gui/%.o: gui/%.c gui/*.h src/*.h | $(BUILD)/gui
	$(CC) $(PROJECT_CFLAGS) $(CFLAGS) $(INC) $(GUI_CFLAGS) -c -o $@ $<

$(BUILD)/%-protocol.o: $(BUILD)/%-protocol.c $(PROTO_HEADERS)
	$(CC) $(PROJECT_CFLAGS) $(CFLAGS) $(INC) $(WL_CFLAGS) -c -o $@ $<

$(BUILD)/%-client-protocol.h: protocol/%.xml | $(BUILD)
	$(WAYLAND_SCANNER) client-header $< $@

$(BUILD)/%-protocol.c: protocol/%.xml | $(BUILD)
	$(WAYLAND_SCANNER) private-code $< $@

$(BUILD) $(BUILD)/gui:
	mkdir -p $@

install: $(TARGETS)
	install -Dm755 driftpaper $(DESTDIR)$(BINDIR)/driftpaper
	sed 's|@BINDIR@|$(BINDIR)|' dist/driftpaper.service.in > $(BUILD)/driftpaper.service
	install -Dm644 $(BUILD)/driftpaper.service $(DESTDIR)$(PREFIX)/lib/systemd/user/driftpaper.service
	install -Dm644 examples/config.ini $(DESTDIR)$(DATADIR)/driftpaper/config.ini
	install -Dm644 LICENSE $(DESTDIR)$(DATADIR)/licenses/driftpaper/LICENSE
	install -Dm644 dist/$(APP_ID).metainfo.xml $(DESTDIR)$(DATADIR)/metainfo/$(APP_ID).metainfo.xml
	install -Dm644 dist/$(APP_ID).svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg
ifeq ($(HAVE_GUI),yes)
	install -Dm755 driftpaper-settings $(DESTDIR)$(BINDIR)/driftpaper-settings
	install -Dm644 dist/$(APP_ID).desktop $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop
endif

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/driftpaper $(DESTDIR)$(BINDIR)/driftpaper-settings
	rm -f $(DESTDIR)$(PREFIX)/lib/systemd/user/driftpaper.service
	rm -f $(DESTDIR)$(DATADIR)/metainfo/$(APP_ID).metainfo.xml $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg
	rm -rf $(DESTDIR)$(DATADIR)/driftpaper $(DESTDIR)$(DATADIR)/licenses/driftpaper

clean:
	rm -rf $(BUILD) driftpaper driftpaper-settings

.PHONY: all install uninstall clean
.SECONDARY: $(PROTOCOLS:%=$(BUILD)/%-protocol.c) $(PROTO_HEADERS)
