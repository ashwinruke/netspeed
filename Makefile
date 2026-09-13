CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_WIN32_WINNT=0x0601
LIBS = -liphlpapi -lgdi32 -luser32 -ladvapi32 -lshell32

netspeed.exe: src/main.c
	$(CC) $(CFLAGS) -g src/main.c -o netspeed.exe $(LIBS)

release: src/main.c
	$(CC) $(CFLAGS) -O2 src/main.c -o netspeed.exe $(LIBS) -mwindows

clean:
	rm -f netspeed.exe