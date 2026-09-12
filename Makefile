CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
LDFLAGS := -lz -lpthread

SRC := src/main.cpp
BIN := http-server

.PHONY: all clean run

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN)

run: all
	./$(BIN)
