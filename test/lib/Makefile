CC ?= gcc
RM ?= rm
CFLAGS = -Wall -fPIC -O2

libdazukofs.so: dazukofs.c dazukofs.h
	$(CC) $(CFLAGS) -shared dazukofs.c -o libdazukofs.so

clean:
	$(RM) -f libdazukofs.so

.PHONY: clean
