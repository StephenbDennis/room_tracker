# Host test build for the pure firmware core.
#
# ld2450.c, fusion.c and zones.c deliberately carry no ESP-IDF dependencies, so
# the three modules most likely to harbour bugs (sign decoding, geometry,
# hysteresis timing) are testable with plain gcc in milliseconds instead of via
# flash-and-squint on hardware.
#
#   make          build and run everything
#   make clean

CC      ?= gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wno-unused-parameter -O1 -g
LDLIBS  := -lm
SRC     := ../main
BUILD   := build

SUITES  := ld2450 zones fusion integration

BINS    := $(addprefix $(BUILD)/test_,$(SUITES))

.PHONY: all test clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_ld2450: test_ld2450.c $(SRC)/ld2450.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_zones: test_zones.c $(SRC)/zones.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_fusion: test_fusion.c $(SRC)/fusion.c $(SRC)/ld2450.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# The one suite that links both, so the seam between them is covered.
$(BUILD)/test_integration: test_integration.c $(SRC)/fusion.c $(SRC)/zones.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(BINS)
	@fail=0; \
	for b in $(BINS); do ./$$b || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAILURES"; exit 1; fi; \
	echo "all suites passed"

clean:
	rm -rf $(BUILD)
