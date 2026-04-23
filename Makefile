CC      = clang
CFLAGS  = -g -O0 -Wall \
           $(shell php-config --includes) \
           $(shell llvm-config --cflags)
LDFLAGS = -L/usr/lib64 -lphp \
           $(shell llvm-config --ldflags --libs core analysis native target) \
           -Wl,-rpath,/usr/lib64

SRC     = src/main.c src/dump.c src/codegen.c
OBJ     = $(SRC:.c=.o)
BIN     = phpc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BIN) test.php
	./$(BIN) test.php

clean:
	rm -f $(OBJ) $(BIN) output.o a.out

.PHONY: all test clean
