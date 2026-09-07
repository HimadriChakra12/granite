.PHONY: build clean watch

build: tools/build
	./tools/build

tools/build: tools/build.c tools/build.h tools/mujscompiler.h
	$(CC) -O2 -Wall -Wextra -Wno-unused-function -o tools/build tools/build.c -lmujs

clean:
	rm -rf dist tools/build

watch: tools/build
	@while true; do \
		./tools/build; \
		inotifywait -qre modify src tools/VERSION > /dev/null; \
	done
