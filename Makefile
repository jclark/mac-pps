all: pollpps

pollpps: pollpps.c

clean:
	-rm -f pollpps

.PHONY: all clean
