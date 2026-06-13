# dfrd — T1 Touch Bar userspace stack (renderer + touch daemon)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-missing-field-initializers
DRM_CFLAGS := $(shell pkg-config --cflags libdrm 2>/dev/null || echo -I/usr/include/libdrm)
DRM_LIBS   := $(shell pkg-config --libs   libdrm 2>/dev/null || echo -ldrm)
FT_CFLAGS  := $(shell pkg-config --cflags freetype2 fontconfig)
FT_LIBS    := $(shell pkg-config --libs   freetype2 fontconfig)

all: dfr-render dfr-touchd dfr-fnd

dfr-render: dfr-render.c dfr-layout.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $(FT_CFLAGS) -o $@ dfr-render.c $(DRM_LIBS) $(FT_LIBS)

dfr-touchd: dfr-touchd.c dfr-layout.h
	$(CC) $(CFLAGS) -o $@ dfr-touchd.c

dfr-fnd: dfr-fnd.c dfr-layout.h
	$(CC) $(CFLAGS) -o $@ dfr-fnd.c

clean:
	rm -f dfr-render dfr-touchd dfr-fnd

.PHONY: all clean
