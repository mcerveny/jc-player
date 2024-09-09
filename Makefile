CFLAGS+=-D__TIMESTAMP_ISO__=$(shell date -u +'"\"%Y-%m-%dT%H:%M:%SZ\""')
CFLAGS+=-DVERSION='"$(shell git describe --tags)"'

#CFLAGS+=-DINFO_DRAW_FINGER=true

OBJS=main.o hid.o disp.o 
TARGET=jc-player

CFLAGS+=-O3
#CFLAGS+=-g -O0

CFLAGS+=-fPIC -DPIC -D_REENTRANT -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -Wall
INCLUDES+=`pkg-config --cflags libdrm` -I/usr/include/freetype2
LDFLAGS+=`pkg-config --libs libdrm` -lavcodec -lavutil -lswresample -lavformat -lfreetype -lpng -lm -lulfius -ljansson

all: $(TARGET) 

$(TARGET): $(OBJS)
	$(CC) -o $@ -Wl,--whole-archive $(OBJS) $(LDFLAGS) -Wl,--no-whole-archive -rdynamic

%.o: %.c
	@rm -f $@ 
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@ -Wno-deprecated-declarations

%.o: %.cpp
	@rm -f $@ 
	$(CXX) $(CFLAGS) $(INCLUDES) -c $< -o $@ -Wno-deprecated-declarations

%.a: $(OBJS)
	$(AR) r $@ $^

clean:
	for i in $(OBJS) $(TARGET); do (if test -e "$$i"; then ( rm $$i ); fi ); done
