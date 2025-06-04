all: pollpps audiopps

pollpps: pollpps.c

audiopps: audiopps.c
	$(CC) -o audiopps audiopps.c -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

clean:
	-rm -f pollpps audiopps

.PHONY: all clean
