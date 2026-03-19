CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g `pkg-config --cflags fuse3`
LDFLAGS = `pkg-config --libs fuse3`

TARGET = mini_unionfs
SRC = mini_unionfs.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
