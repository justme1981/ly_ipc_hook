CC=mipsel-linux-gnu-gcc

CFLAGS=-O2 -march=mips32 -mabi=32 -fPIC
LDFLAGS=-shared -ldl -lpthread

all:
	$(CC) $(CFLAGS) hook_rtsp_full.c -o hook_rtsp.so $(LDFLAGS)
