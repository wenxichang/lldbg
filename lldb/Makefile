SRCS    = $(wildcard *.c)
OBJS    = $(SRCS:.c=.o)
CFLAGS  = -g -O2 -Wall -DOS_LINUX -fPIC
CXXFLAGS = $(CFLAGS)
LDFLAGS = -shared -g -llua
TARGET  = lldb.so

.PHONY: clean install test

$(TARGET): $(OBJS)
		$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

clean:
		rm -f $(TARGET) $(OBJS)

