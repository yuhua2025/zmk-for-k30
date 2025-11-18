# 简单的Makefile用于编译LED控制代码
CC = gcc
CFLAGS = -Wall -Wextra -DTEST_BUILD
TARGET = ble_status_led_test
SRC = ble_status_led.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

test: $(TARGET)
	./$(TARGET)

.PHONY: all clean test