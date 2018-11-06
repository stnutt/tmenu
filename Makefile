.PHONY: install uninstall

PREFIX ?= /usr

install:
	install -Dm 755 tmenu $(PREFIX)/bin/tmenu

uninstall:
	rm -f $(PREFIX)/bin/tmenu
