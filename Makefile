CC = clang
CFLAGS = -std=c17 -O2 -g -Wall -Wextra -Werror -Ibuild $(shell pkg-config --cflags libusb-1.0)
LDLIBS = $(shell pkg-config --libs libusb-1.0)
LIBUB530 ?= /private/tmp/claude-501/-Users-mintopia-Projects-personal-elgato/2c7b928e-fa49-440c-8f94-35fb4c01662c/scratchpad/egc/x/Game Capture HD.app/Contents/Frameworks/libUB530.dylib

all: build/hd60s

build/ub530_tables.h: tools/extract-tables.py
	@mkdir -p build
	python3 -I tools/extract-tables.py "$(LIBUB530)" > $@.tmp && mv $@.tmp $@

build/hd60s: src/main.c src/device.c src/parser.c src/device.h src/parser.h build/ub530_tables.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/device.c src/parser.c $(LDLIBS)

build/test_parser: tests/test_parser.c src/parser.c src/parser.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_parser.c src/parser.c

test: build/test_parser
	build/test_parser

clean:
	rm -rf build

.PHONY: all test clean
