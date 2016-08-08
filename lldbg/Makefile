SRCS    = $(wildcard *.c)
OBJS    = $(SRCS:.c=.o)
CFLAGS  = -g -O2 -Wall -DOS_LINUX
CXXFLAGS = $(CFLAGS)
LDFLAGS =
TARGET  = lldbg

.PHONY: clean install test

$(TARGET): $(OBJS)
		$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

clean:
		rm -f $(TARGET) $(OBJS)

install:
