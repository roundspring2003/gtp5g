CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

TARGET := mark_abi_test

.PHONY: all run clean

all: $(TARGET)

$(TARGET): mark_abi_test.c ../include/gtp5g_mark.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I../include -o $@ mark_abi_test.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
