# Host unit tests for the ESP-IDF-free parts of the SDK.
#
#   make -C test/host test       build and run
#   make -C test/host clean
#
# Needs only a host C99 compiler (CC=gcc by default; CC=clang works).

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -O0 -g
SRC_DIR := ../../src

TESTS := test_module_auth

.PHONY: all test clean

all: $(TESTS)

test_module_auth: test_module_auth.c $(SRC_DIR)/egos_module_auth.c $(SRC_DIR)/egos_module_auth.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ test_module_auth.c $(SRC_DIR)/egos_module_auth.c

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(TESTS) $(addsuffix .exe,$(TESTS))
