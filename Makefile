CORE_NAME := vlc_libretro
CC = gcc

VLC_CFLAGS := $(shell pkg-config --cflags libvlc)
VLC_LIBS   := $(shell pkg-config --libs libvlc)

CFLAGS := -O2 -fPIC -Wall -I. $(VLC_CFLAGS)
LDFLAGS := -shared -static-libgcc -static-libstdc++ 
LIBS := -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic $(VLC_LIBS)

TARGET := $(CORE_NAME).dll
OBJS := vlc_core.o vlc_video.o vlc_audio.o vlc_menu.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o *.dll