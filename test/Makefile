CC ?= gcc
MAKE ?= make
RM ?= rm
CFLAGS = -Wall -fPIC -O2 -Ilib
LDFLAGS = -Llib

showfiles: showfiles.c lib/libdazukofs.so
	$(CC) $(CFLAGS) $(LDFLAGS) showfiles.c -ldazukofs -o showfiles

lib/libdazukofs.so:
	$(MAKE) -C lib

clean:
	$(MAKE) -C lib clean
	$(RM) -f showfiles

.PHONY: clean
