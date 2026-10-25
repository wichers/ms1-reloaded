# Test programs for both masters: make test
CC     ?= cc
CFLAGS ?= -std=c99 -Wall -O2

build/ms1_test_trace: test/ms1_test_trace.c src/ms1_types.c src/ms1_master.c include/ms1_*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -o $@ test/ms1_test_trace.c src/ms1_types.c src/ms1_master.c

build/cs2_test: test/cs2_test.c src/cs2_types.c src/cs2_master.c include/cs2_*.h include/ms1_platform.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -o $@ test/cs2_test.c src/cs2_types.c src/cs2_master.c

test: build/ms1_test_trace build/cs2_test
	build/ms1_test_trace
	build/cs2_test

clean:
	rm -rf build

.PHONY: test clean
