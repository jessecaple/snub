CC ?= gcc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: all clean test

all: snub

snub: main.o filter.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test-filter: test_filter.o filter.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

main.o filter.o test_filter.o: filter.h

test: test-filter
	./test-filter

clean:
	rm -f snub test-filter *.o
