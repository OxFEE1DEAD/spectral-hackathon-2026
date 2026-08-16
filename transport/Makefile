# Build for the sender/receiver transport. Depends only on a C++17 compiler and
# librt, same as the harness it plugs into.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -I../harness/include
LDLIBS   ?= -lrt -lpthread

BIN := bin
BINARIES := $(BIN)/sender $(BIN)/receiver

.PHONY: all clean test

all: $(BINARIES)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/sender: src/sender.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BIN)/receiver: src/receiver.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

test: $(BIN)/test_transport
	./$(BIN)/test_transport

$(BIN)/test_transport: test/test_transport.cpp include/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -rf $(BIN)
