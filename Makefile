CXX      := g++
CXXFLAGS := -Wall -Wno-unused-function -std=c++17
FLEX     := flex

SRC_DIR   := src
BUILD_DIR := build
LEX_FILE  := $(SRC_DIR)/lexer.l
GEN_FILE  := $(BUILD_DIR)/lex.yy.c
TARGET    := lexer

.PHONY: all clean run

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(GEN_FILE): $(LEX_FILE) | $(BUILD_DIR)
	$(FLEX) -o $(GEN_FILE) $(LEX_FILE)

$(TARGET): $(GEN_FILE)
	$(CXX) $(CXXFLAGS) -x c++ $(GEN_FILE) -o $(TARGET) -lfl

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

run: $(TARGET)
	./$(TARGET) $(FILE)
