# dfrd — T1 Touch Bar userspace stack (renderer + touch daemon)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
DRM_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm)
DRM_LIBS   := $(shell pkg-config --libs   libdrm 2>/dev/null || echo -ldrm)

all: dfr-render dfr-touchd

dfr-render: dfr-render.c dfr-layout.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) -o $@ dfr-render.c $(DRM_LIBS)

dfr-touchd: dfr-touchd.c dfr-layout.h
	$(CC) $(CFLAGS) -o $@ dfr-touchd.c

clean:
	rm -f dfr-render dfr-touchd

.PHONY: all clean
