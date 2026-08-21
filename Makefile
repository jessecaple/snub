CC ?= gcc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all clean test

all: xbox-snapback-filter

xbox-snapback-filter: main.o filter.o
	$(CC) $(CFLAGS) -o $@ $^

test-filter: test_filter.o filter.o
	$(CC) $(CFLAGS) -o $@ $^

test: test-filter
	./test-filter

clean:
	rm -f xbox-snapback-filter test-filter *.o
