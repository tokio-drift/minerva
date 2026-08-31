CXX      := g++
CXXFLAGS := -Wall -Wno-unused-function -std=c++17
FLEX     := flex
BISON    := bison

SRC_DIR   := src
BUILD_DIR := build
LEX_FILE  := $(SRC_DIR)/lexer.l
PARSER_FILE := $(SRC_DIR)/parser.y
LEX_GEN   := $(BUILD_DIR)/lex.yy.cpp
PARSER_GEN := $(BUILD_DIR)/parser.tab.cpp
PARSER_HDR := $(BUILD_DIR)/parser.tab.h
TARGET    := minerva

.PHONY: all clean run

all: $(TARGET)

build: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PARSER_GEN): $(PARSER_FILE)
	mkdir -p $(BUILD_DIR)
	$(BISON) -d --defines=$(PARSER_HDR) -o $(PARSER_GEN) $(PARSER_FILE)

$(PARSER_HDR): $(PARSER_GEN)

$(LEX_GEN): $(LEX_FILE) $(PARSER_HDR)
	$(FLEX) -o $(LEX_GEN) $(LEX_FILE)

$(TARGET): $(LEX_GEN) $(PARSER_GEN)
	$(CXX) $(CXXFLAGS) $(LEX_GEN) $(PARSER_GEN) -o $(TARGET) -lfl

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

run: $(TARGET)
	./$(TARGET) $(FILE)
