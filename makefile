objs := tcpbwd.o
CFLAGS += -Wall
LDFLAGS += -pthread

tcpbwd: ${objs}
