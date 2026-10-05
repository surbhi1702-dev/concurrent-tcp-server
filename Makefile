CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude
BIN      = bin

HEADERS  = include/net_utils.h include/server_common.h include/thread_pool.h

all: $(BIN)/basic_server $(BIN)/pool_server $(BIN)/client

$(BIN):
	mkdir -p $(BIN)

$(BIN)/basic_server: src/basic_server.cpp $(HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BIN)/pool_server: src/pool_server.cpp $(HEADERS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BIN)/client: src/client.cpp include/net_utils.h | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

benchmark: all
	./scripts/benchmark.sh

clean:
	rm -rf $(BIN)

.PHONY: all clean benchmark
