CC ?= gcc
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
INC = -Isysroot/usr/include
LIBS = -Llinklib -lmpv -lEGL -lGLESv2 -lX11 -lm -ldl -lpthread
SRC = src/mpv_compare.c

mpv-compare: $(SRC) src/stb_truetype.h
	$(CC) $(CFLAGS) $(INC) $(SRC) -o $@ $(LIBS)

clean:
	rm -f mpv-compare

.PHONY: clean
