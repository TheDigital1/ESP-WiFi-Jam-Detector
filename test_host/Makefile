# SPDX-License-Identifier: GPL-3.0-or-later
# Host unit tests for the IDF-free modules. Run: make -C test_host
CC ?= cc
CFLAGS ?= -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror
INC = -I../main -I../components/espsdr_rx/upstream/common
BUILD = build

TESTS = $(BUILD)/test_dsp

.PHONY: all test clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_dsp: test_dsp.c ../main/dsp.c ../main/dsp.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test_dsp.c ../main/dsp.c -lm

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -rf $(BUILD)
