# SPDX-License-Identifier: GPL-3.0-or-later
# Host unit tests for the IDF-free modules. Run: make -C test_host
CC ?= cc
CFLAGS ?= -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror
INC = -I../main -I../components/espsdr_rx/upstream/common
BUILD = build

TESTS = $(BUILD)/test_dsp $(BUILD)/test_bandplan $(BUILD)/test_plot
SIM = $(BUILD)/sim/.stamp

.PHONY: all test clean
all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_dsp: test_dsp.c ../main/dsp.c ../main/dsp.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test_dsp.c ../main/dsp.c -lm

$(BUILD)/test_bandplan: test_bandplan.c ../main/bandplan.c ../main/bandplan.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test_bandplan.c ../main/bandplan.c

$(BUILD)/test_plot: test_plot.c ../main/plot.c ../main/plot.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test_plot.c ../main/plot.c

$(BUILD)/test_detect: test_detect.c ../main/detect.c ../main/detect.h ../main/dsp.c ../main/bandplan.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test_detect.c ../main/detect.c ../main/dsp.c ../main/bandplan.c -lm

# Synthetic spectra (deterministic seeds); regenerated when the generator changes.
$(SIM): ../tools/sim_spectra.py | $(BUILD)
	python3 ../tools/sim_spectra.py --out $(BUILD)/sim
	touch $@

test: $(TESTS) $(BUILD)/test_detect $(SIM)
	@for t in $(TESTS); do ./$$t || exit 1; done
	./$(BUILD)/test_detect $(BUILD)/sim

clean:
	rm -rf $(BUILD)
