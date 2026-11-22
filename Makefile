CC = clang
CFLAGS = -std=c17 -O2 -g -Wall -Wextra -Werror -Ibuild $(shell pkg-config --cflags libusb-1.0)
LDLIBS = $(shell pkg-config --libs libusb-1.0)
EGC_URL = https://edge.elgato.com/egc/macos/egcm/2.11.14/final/Game_Capture_HD_2.11.14.zip
VERSION ?= $(patsubst v%,%,$(shell git describe --tags --abbrev=0 2>/dev/null || echo v0.0.0))
# macOS only replaces a system extension when its build number rises.
BUILD ?= $(shell git rev-list --count HEAD)
export DEVELOPER_DIR ?= /Applications/Xcode.app/Contents/Developer
XCODEBUILD = xcodebuild -project mac/HD60S.xcodeproj \
	-configuration Release MARKETING_VERSION=$(VERSION) CURRENT_PROJECT_VERSION=$(BUILD) $(XCODEFLAGS)

all: build/hd60s

build/libUB530.dylib:
	@mkdir -p build
	curl -fsSL -o build/egc.zip $(EGC_URL)
	unzip -p build/egc.zip "Game Capture HD.app/Contents/Frameworks/libUB530.dylib" > $@.tmp && mv $@.tmp $@
	rm build/egc.zip

build/ub530_tables.h: tools/extract-tables.py build/libUB530.dylib
	python3 -I tools/extract-tables.py build/libUB530.dylib > $@.tmp && mv $@.tmp $@

build/hd60s: src/main.c src/device.c src/parser.c src/device.h src/parser.h build/ub530_tables.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/device.c src/parser.c $(LDLIBS)

build/test_parser: tests/test_parser.c src/parser.c src/parser.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_parser.c src/parser.c

app: build/ub530_tables.h
	cd mac && xcodegen generate --quiet
	$(XCODEBUILD) -scheme HD60S -derivedDataPath build/xcode -allowProvisioningUpdates -quiet build
	rm -rf build/HD60S.app && cp -R build/xcode/Build/Products/Release/HD60S.app build/

# Development-signed build with the AudioDriverKit extension (needs the DriverKit dev profile; not notarisable).
DEXT_CONFIG ?= Debug
dext-dev: build/ub530_tables.h
	cd mac && xcodegen generate --quiet --spec project-dext.yml
	xcodebuild -project mac/HD60SDev.xcodeproj -configuration $(DEXT_CONFIG) MARKETING_VERSION=$(VERSION) CURRENT_PROJECT_VERSION=$(BUILD) $(XCODEFLAGS) \
		-scheme HD60S -derivedDataPath build/xcode-dext -allowProvisioningUpdates -quiet build
	rm -rf build/HD60S-dext.app && cp -R build/xcode-dext/Build/Products/$(DEXT_CONFIG)/HD60S.app build/HD60S-dext.app

install-app: app
	-killall HD60S 2>/dev/null
	rm -rf /Applications/HD60S.app && cp -R build/HD60S.app /Applications/
	open /Applications/HD60S.app --args --install

# dmg APP OUTPUT: packs APP as HD60S.app into a DMG.
dmg = rm -rf build/dist && mkdir -p build/dist && cp -R $(1) build/dist/HD60S.app && \
	hdiutil create -quiet -volname "HD60S $(VERSION)" -srcfolder build/dist -ov -format UDZO $(2)

# Userspace flavour (Developer ID, notarised in CI).
dist: app
	$(call dmg,build/HD60S.app,build/HD60S-$(VERSION).dmg)

# DriverKit flavour (Apple Development, never notarised).
dist-dext: DEXT_CONFIG = Release
dist-dext: dext-dev
	$(call dmg,build/HD60S-dext.app,build/HD60S-DriverKit-$(VERSION).dmg)

build/test_ring: tests/test_ring.c src/ring.c src/ring.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_ring.c src/ring.c

build/test_audio_plugin: tests/test_audio_plugin.swift mac/App/AudioPlugin.swift mac/App/DextStatus.swift
	@mkdir -p build
	swiftc -parse-as-library -o $@ mac/App/AudioPlugin.swift mac/App/DextStatus.swift tests/test_audio_plugin.swift

build/test_tone: tests/test_tone.c mac/Dext/Tone.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ tests/test_tone.c -lm

build/test_plugin: tests/test_plugin.c mac/Audio/HD60SAudio.c
	@mkdir -p build
	$(CC) $(CFLAGS) -Wno-four-char-constants -fsanitize=address,undefined -o $@ tests/test_plugin.c -framework CoreFoundation

test: build/test_parser build/test_ring build/test_plugin build/test_audio_plugin build/test_tone
	build/test_parser
	build/test_ring
	build/test_plugin
	build/test_audio_plugin
	build/test_tone

clean:
	rm -rf build

.PHONY: all app dext-dev dist dist-dext install-app test clean
