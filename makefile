objs   := tcpbwd.o
CFLAGS += -Wall -g
LDFLAGS += -pthread

tcpbwd: ${objs}
