CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
CURL_LIB ?= /usr/lib/x86_64-linux-gnu/libcurl.so.4

.PHONY: all test sanitize preview preview-adaptive clean
all: build/screen

build:
	mkdir -p build

build/screen: aviato-screen.c stb_truetype.h stb_image.h json-reader.h quotes.h portfolio-cache.h display-handover.h ambient-screen.h minimal-screen.h spotify-screen.h adaptive-screen.h | build
	$(CC) $(CFLAGS) -s -o $@ aviato-screen.c -lm $(CURL_LIB)

build/test-screen: tests/test-screen.c aviato-screen.c stb_truetype.h stb_image.h json-reader.h quotes.h portfolio-cache.h display-handover.h ambient-screen.h minimal-screen.h spotify-screen.h adaptive-screen.h | build
	$(CC) $(CFLAGS) -g -o $@ tests/test-screen.c -lm

test: build/test-screen
	./build/test-screen

sanitize: | build
	$(CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test-screen-sanitize tests/test-screen.c -lm
	ASAN_OPTIONS=detect_leaks=1 ./build/test-screen-sanitize

preview: build/screen
	SCREEN_OFFLINE=1 SCREEN_FORCE_HOUR=12 SCREEN_PREVIEW=build/day.bmp ./build/screen
	SCREEN_OFFLINE=1 SCREEN_FORCE_HOUR=23 SCREEN_PREVIEW=build/night.bmp ./build/screen
	SCREEN_OFFLINE=1 SCREEN_FORCE_HOUR=2 SCREEN_PREVIEW=build/clock.bmp ./build/screen

build/preview-adaptive: tests/preview-adaptive.c aviato-screen.c stb_truetype.h stb_image.h json-reader.h quotes.h portfolio-cache.h display-handover.h ambient-screen.h minimal-screen.h spotify-screen.h adaptive-screen.h | build
	$(CC) $(CFLAGS) -o $@ tests/preview-adaptive.c -lm $(CURL_LIB)

preview-adaptive: build/preview-adaptive
	./build/preview-adaptive "$(COVER)"

clean:
	rm -rf build
