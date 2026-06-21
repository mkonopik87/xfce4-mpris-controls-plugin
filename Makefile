PLUGIN := mpris-controls
PREFIX ?= /usr

CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS += -fPIC -Wall -Wextra -O2
LDFLAGS += -shared

REQUIRED_PACKAGES := gtk+-3.0 libxfce4ui-2 libxfce4panel-2.0
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(REQUIRED_PACKAGES) 2>/dev/null)
LDLIBS += $(shell $(PKG_CONFIG) --libs $(REQUIRED_PACKAGES) 2>/dev/null)

LIBDIR ?= $(shell $(PKG_CONFIG) --variable=libdir libxfce4panel-2.0 2>/dev/null)
DATADIR ?= $(PREFIX)/share
PLUGIN_DIR := $(LIBDIR)/xfce4/panel/plugins
DESKTOP_DIR := $(DATADIR)/xfce4/panel/plugins

TARGET := lib$(PLUGIN).so
SOURCES := src/$(PLUGIN).c

.PHONY: all check-deps clean install uninstall

all: check-deps $(TARGET)

check-deps:
	@missing=""; \
	for package in $(REQUIRED_PACKAGES); do \
		$(PKG_CONFIG) --exists "$$package" || missing="$$missing $$package"; \
	done; \
	if [ -n "$$missing" ]; then \
		echo "Missing build dependencies:$$missing"; \
		echo "Debian/Ubuntu: sudo apt install build-essential pkg-config libxfce4panel-2.0-dev libxfce4ui-2-dev libgtk-3-dev"; \
		echo "Fedora: sudo dnf install gcc make pkgconf-pkg-config xfce4-panel-devel gtk3-devel"; \
		exit 1; \
	fi

$(TARGET): check-deps $(SOURCES)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

install: check-deps $(TARGET)
	install -d "$(DESTDIR)$(PLUGIN_DIR)" "$(DESTDIR)$(DESKTOP_DIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"
	install -m 644 "data/$(PLUGIN).desktop" "$(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN).desktop"

uninstall:
	rm -f "$(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"
	rm -f "$(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN).desktop"

clean:
	rm -f "$(TARGET)"
