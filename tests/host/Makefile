CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O0 -g
INCLUDES = -I../../main

SRCS = test_host.cpp ../../main/failsafe.cpp ../../main/mavlink_min.cpp ../../main/mesh_frame.cpp
BIN = test_host

.PHONY: all clean test

all: test

$(BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(BIN) $(SRCS) -lm

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
