CC = gcc
WINDRES = windres
CFLAGS = -Wall -Wextra -std=c11 -D_WIN32_WINNT=0x0601
LIBS = -liphlpapi -lgdi32 -luser32 -ladvapi32 -lshell32

netspeed.exe: src/main.c resource.o
	$(CC) $(CFLAGS) -g src/main.c resource.o -o netspeed.exe $(LIBS)

release: src/main.c resource.o
	$(CC) $(CFLAGS) -O2 src/main.c resource.o -o netspeed.exe $(LIBS) -mwindows

resource.o: src/netspeed.rc netspeed.ico
	$(WINDRES) src/netspeed.rc -O coff -o resource.o

clean:
	del /q netspeed.exe resource.o 2>nul