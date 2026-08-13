# Standalone build for the fan-out benchmark harness. No external dependencies
# beyond a C++17 compiler and librt (POSIX shared memory).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
LDLIBS   ?= -lpthread

# shm_open lives in librt on glibc older than 2.34 (Ubuntu 20.04 is 2.31), and
# in libc from 2.34 on (Ubuntu 24.04 is 2.39). Linking it on Linux covers both
# of the target distributions; macOS has no librt at all.
ifeq ($(shell uname -s),Linux)
LDLIBS   += -lrt
endif

BIN := bin
BINARIES := $(BIN)/producer $(BIN)/consumer

.PHONY: all clean test

all: $(BINARIES)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/producer: src/producer.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/consumer: src/consumer.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

test: $(BIN)/test_harness
	./$(BIN)/test_harness

$(BIN)/test_harness: test/test_harness.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf $(BIN)
