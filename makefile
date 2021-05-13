CC = gcc
CFLAGS = -g -pthread -Wall -lrt
TARGET = projeto

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c

clean:
		$(RM) $(TARGET)
