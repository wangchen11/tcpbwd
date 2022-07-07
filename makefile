objs   := tcpbwd.o
CFLAGS += -Wall -g -std=c99 -D_GNU_SOURCE
LDFLAGS += -pthread

tcpbwd: ${objs}

clean:
	rm -vf ${objs} tcpbwd