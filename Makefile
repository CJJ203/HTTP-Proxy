CC = gcc
CFLAGS = -g -Wall -Werror

all: proxy

proxy: proxy.o
	$(CC) $(CFLAGS) proxy.o -o proxy

proxy.o:
	$(CC) $(CFLAGS) -c proxy.c

clean:
	rm -rf *.o
