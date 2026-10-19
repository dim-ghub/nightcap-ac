CC ?= cc
CFLAGS ?= -O2 -g
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
PREFIX ?= $(HOME)/.local

HOOK := build/libnightcap_ac_hook.so

.PHONY: all clean test install uninstall

all: $(HOOK)

# The hook only needs SDL3's headers; the real SDL comes from the process it
# is preloaded into, so it is deliberately not linked against libSDL3.
$(HOOK): hook/nightcap_ac_hook.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -std=c11 -Wall -Wextra -fPIC -shared \
		-fvisibility=hidden -o $@ $< -ldl -lpthread

test: $(HOOK)
	python3 -m unittest discover -s tests -v

install: $(HOOK)
	install -Dm755 $(HOOK) $(PREFIX)/lib/nightcap-ac/libnightcap_ac_hook.so
	install -Dm755 scripts/nightcap-ac $(PREFIX)/bin/nightcap-ac
	install -Dm644 packaging/nightcap-ac.desktop $(PREFIX)/share/applications/nightcap-ac.desktop
	@# Older versions installed a separate launcher entry; Nightcap AC does both now.
	rm -f $(PREFIX)/share/applications/nightcap-ac-roblox.desktop
	mkdir -p $(PREFIX)/share/nightcap-ac
	rm -rf $(PREFIX)/share/nightcap-ac/nightcap_ac
	cp -r nightcap_ac $(PREFIX)/share/nightcap-ac/
	@# Asks y/n before touching Nightcap's launcher (NIGHTCAP_AC_DESKTOP=y|n skips the question).
	@$(PREFIX)/bin/nightcap-ac desktop ask --exec $(PREFIX)/bin/nightcap-ac

uninstall:
	-@$(PREFIX)/bin/nightcap-ac desktop remove
	rm -f $(PREFIX)/bin/nightcap-ac
	rm -rf $(PREFIX)/lib/nightcap-ac $(PREFIX)/share/nightcap-ac
	rm -f $(PREFIX)/share/applications/nightcap-ac.desktop
	rm -f $(PREFIX)/share/applications/nightcap-ac-roblox.desktop

clean:
	rm -rf build
