all: pollpps audiopps

pollpps: pollpps.c chrony_client.c chrony_client.h
	$(CC) -o pollpps pollpps.c chrony_client.c

audiopps: audiopps.c chrony_client.c chrony_client.h
	$(CC) -o audiopps audiopps.c chrony_client.c -framework CoreAudio -framework AudioToolbox -framework CoreFoundation

clean:
	-rm -f pollpps audiopps

.PHONY: all clean
