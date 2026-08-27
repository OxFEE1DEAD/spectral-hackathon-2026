# Measurement tools, kept apart from the transport they measure. Same dependency-free
# build: a C++17 compiler and librt, nothing else.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -I../transport/include -I../harness/include
LDLIBS   ?= -lrt -lpthread

BIN := bin
BINARIES := $(BIN)/clock_probe

.PHONY: all clean

all: $(BINARIES)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/clock_probe: clock_probe.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf $(BIN)
