CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
PREFIX  ?= /usr/local

SRC = src/main.c src/util.c src/vt.c src/server.c src/client.c src/manager.c
OBJ = $(SRC:.c=.o)

gtm: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c src/gtm.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: gtm
	install -Dm755 gtm $(DESTDIR)$(PREFIX)/bin/gtm

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/gtm

clean:
	rm -f gtm $(OBJ)

.PHONY: install uninstall clean
