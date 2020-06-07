CFLAGS += -std=gnu11 -Wall
LDLIBS += -lxcb

ifdef DEBUG
CFLAGS += -DDEBUG=1
endif

.PHONY: build run clean

build: pointer-lock

run: build
	./pointer-lock

clean:
	rm -rfv pointer-lock
