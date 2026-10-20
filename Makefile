CXX ?= c++
CXXFLAGS ?= -O2 -g
PREFIX ?= $(HOME)/.local

SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

LIB := build/libnightcap_ac.so
SRC := $(wildcard src/*.cc)
IMGUI := third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
	third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
	third_party/imgui/imgui_impl_vulkan.cpp
OBJ := $(SRC:src/%.cc=build/obj/%.o) $(IMGUI:third_party/imgui/%.cpp=build/obj/imgui/%.o)

# Everything is hidden except the SDL wrappers and the Vulkan layer entry
# point, so nothing else in Nightcap can bind to our copies.
FLAGS := -std=c++17 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden \
	-DVK_NO_PROTOTYPES -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
	-Ithird_party/imgui $(SDL_CFLAGS)

.PHONY: all build clean test install uninstall patch-desktop unpatch-desktop

all: build

build: $(LIB) build/nightcap-ac-render

build/obj/%.o: src/%.cc $(wildcard src/*.h src/*.inc)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FLAGS) -Wall -Wextra -Wno-missing-field-initializers -c -o $@ $<

build/obj/imgui/%.o: third_party/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FLAGS) -w -c -o $@ $<

$(LIB): $(OBJ) src/exports.map
	$(CXX) -shared -o $@ $(OBJ) $(SDL_LIBS) -ldl -lpthread -Wl,--no-undefined \
		-Wl,--version-script=src/exports.map

build/nightcap-ac-render: tools/render.cc src/synth.cc src/synth.h
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -Wextra -o $@ tools/render.cc src/synth.cc

TEST_BINS := build/vk_driver build/mic_driver build/chat_test

build/vk_driver: tests/vk_driver.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $< $(SDL_CFLAGS) $(SDL_LIBS) -lvulkan

build/mic_driver: tests/mic_driver.cc
	$(CXX) $(CXXFLAGS) -std=c++17 -o $@ $< $(SDL_CFLAGS) $(SDL_LIBS)

build/chat_test: tests/chat_test.cc src/chat.cc src/chat.h
	$(CXX) $(CXXFLAGS) -std=c++17 -Wall -o $@ tests/chat_test.cc src/chat.cc

test: build $(TEST_BINS)
	./build/chat_test
	python3 -m unittest discover -s tests -v

# --- install -------------------------------------------------------------------

BINDIR := $(PREFIX)/bin
DATA_HOME := $(or $(XDG_DATA_HOME),$(HOME)/.local/share)
DESKTOP_ID := io.github.CoderDayton.nightcap.desktop
DESKTOP_TARGET := $(DATA_HOME)/applications/$(DESKTOP_ID)
# Nightcap's own launcher entry (native first, then Flatpak).
DESKTOP_SOURCE ?= $(firstword $(wildcard \
	/usr/local/share/applications/$(DESKTOP_ID) \
	/usr/share/applications/$(DESKTOP_ID) \
	$(DATA_HOME)/flatpak/exports/share/applications/$(DESKTOP_ID) \
	/var/lib/flatpak/exports/share/applications/$(DESKTOP_ID)))
MARKER := X-Nightcap-AC=true

install: build
	install -Dm755 $(LIB) $(PREFIX)/lib/nightcap-ac/libnightcap_ac.so
	strip --strip-debug $(PREFIX)/lib/nightcap-ac/libnightcap_ac.so
	install -Dm755 scripts/nightcap-ac $(BINDIR)/nightcap-ac
	install -Dm644 packaging/nightcap_ac_layer.json.in $(PREFIX)/share/nightcap-ac/nightcap_ac_layer.json.in
	install -Dm644 packaging/nightcap-ac.desktop $(PREFIX)/share/applications/nightcap-ac.desktop
	@# Leftovers from the standalone (Python + virtual mic) version; closing
	@# it puts Nightcap's mic back and removes its virtual devices.
	-@pkill -TERM -f '^/usr/bin/python3 -m nightcap_ac' && echo "closed the old Nightcap AC app" || true
	rm -rf $(PREFIX)/share/nightcap-ac/nightcap_ac $(PREFIX)/lib/nightcap-ac/libnightcap_ac_hook.so
	rm -f $(PREFIX)/share/applications/nightcap-ac-roblox.desktop
	@# Offer to patch Nightcap's own launcher entry (NIGHTCAP_AC_DESKTOP=y|n
	@# answers for scripted installs). An existing patch is just refreshed.
	@if grep -qxF '$(MARKER)' '$(DESKTOP_TARGET)' 2>/dev/null; then \
	  $(MAKE) --no-print-directory patch-desktop; \
	else \
	  answer='$(NIGHTCAP_AC_DESKTOP)'; \
	  if [ -z "$$answer" ] && [ -t 0 ]; then \
	    echo; \
	    echo "Patch Nightcap's launcher so the Nightcap app entry and roblox:// links"; \
	    echo "(joining from the website) start Nightcap with Nightcap AC?"; \
	    echo "This writes a copy of Nightcap's entry to $(DESKTOP_TARGET)."; \
	    echo "Undo any time with 'make unpatch-desktop'."; \
	    printf 'Patch it? [y/N] '; read -r answer; \
	  fi; \
	  case "$$answer" in \
	    y|Y|yes) $(MAKE) --no-print-directory patch-desktop ;; \
	    *) echo "Skipped the launcher patch. Run 'make patch-desktop' to add it later." ;; \
	  esac; \
	fi

# Copies Nightcap's entry with its main Exec going through nightcap-ac. The
# Settings action is kept; X-Mocktail-Managed is dropped so Nightcap's
# register_url_handler.sh won't overwrite the copy. An entry you had there
# yourself is backed up and comes back with unpatch-desktop.
patch-desktop:
	@[ -n '$(DESKTOP_SOURCE)' ] || { echo "Nightcap's desktop entry was not found; is Nightcap installed?"; exit 1; }
	@mkdir -p '$(dir $(DESKTOP_TARGET))'
	@if [ -e '$(DESKTOP_TARGET)' ] && ! grep -qxF '$(MARKER)' '$(DESKTOP_TARGET)'; then \
	  cp -p '$(DESKTOP_TARGET)' '$(DESKTOP_TARGET).nightcap-ac-backup'; \
	fi
	@awk -v launcher='$(BINDIR)/nightcap-ac' -v marker='$(MARKER)' ' \
	  /^\[/ { group = $$0; print; if (group == "[Desktop Entry]") print marker; next } \
	  group == "[Desktop Entry]" && ($$0 == "X-Mocktail-Managed=true" || $$0 == marker) { next } \
	  group == "[Desktop Entry]" && /^Exec=/ && !done { print "Exec=env SDL_VIDEODRIVER=wayland,x11 " launcher " %u"; done = 1; next } \
	  { print }' '$(DESKTOP_SOURCE)' > '$(DESKTOP_TARGET).tmp'
	@chmod 644 '$(DESKTOP_TARGET).tmp' && mv '$(DESKTOP_TARGET).tmp' '$(DESKTOP_TARGET)'
	@-update-desktop-database -q '$(dir $(DESKTOP_TARGET))' 2>/dev/null || true
	@-xdg-mime default $(DESKTOP_ID) x-scheme-handler/roblox x-scheme-handler/roblox-player 2>/dev/null || true
	@echo "Patched $(DESKTOP_TARGET) (from $(DESKTOP_SOURCE))"

unpatch-desktop:
	@if [ ! -e '$(DESKTOP_TARGET)' ]; then echo "No launcher patch installed"; \
	elif ! grep -qxF '$(MARKER)' '$(DESKTOP_TARGET)'; then echo "Left $(DESKTOP_TARGET) alone: not written by Nightcap AC"; \
	else \
	  rm -f '$(DESKTOP_TARGET)'; \
	  if [ -e '$(DESKTOP_TARGET).nightcap-ac-backup' ]; then \
	    mv '$(DESKTOP_TARGET).nightcap-ac-backup' '$(DESKTOP_TARGET)'; echo "Restored your previous $(DESKTOP_TARGET)"; \
	  else echo "Removed $(DESKTOP_TARGET); the system Nightcap entry is used again"; fi; \
	  update-desktop-database -q '$(dir $(DESKTOP_TARGET))' 2>/dev/null || true; \
	fi

uninstall: unpatch-desktop
	rm -f $(BINDIR)/nightcap-ac $(PREFIX)/share/applications/nightcap-ac.desktop
	rm -rf $(PREFIX)/lib/nightcap-ac $(PREFIX)/share/nightcap-ac

clean:
	rm -rf build
