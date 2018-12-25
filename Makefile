CC ?= cc
CLFAGS = -pedantic -Wall -Wextra -Os

PREFIX ?= /usr

SRC = tmenu.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean install uninstall

all: tmenu

tmenu: tmenu.o
	$(CC) $(CLFAGS) $< -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJ) tmenu

install:
	install -Dm 755 tmenu $(PREFIX)/bin/tmenu

uninstall:
	rm -f $(PREFIX)/bin/tmenu
