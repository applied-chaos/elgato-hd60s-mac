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

app: build/ub530_tables.h
	cd mac && xcodegen generate --quiet
	DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild -project mac/HD60S.xcodeproj \
		-scheme HD60S -configuration Release -derivedDataPath build/xcode -allowProvisioningUpdates -quiet build
	rm -rf build/HD60S.app && cp -R build/xcode/Build/Products/Release/HD60S.app build/

install-app: app
	rm -rf /Applications/HD60S.app && cp -R build/HD60S.app /Applications/
	open /Applications/HD60S.app

test: build/test_parser
	build/test_parser

clean:
	rm -rf build

.PHONY: all app install-app test clean
