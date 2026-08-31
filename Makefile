CXX      := g++
CXXFLAGS := -Wall -Wno-unused-function -std=c++17 -Ibuild

# OS Detection for Cross-Platform Compatibility
ifeq ($(OS),Windows_NT)
    FLEX      := win_flex
    BISON     := win_bison
    TARGET    := compiler.exe
    MKDIR_CMD := if not exist build mkdir build
    CLEAN_CMD := if exist build rmdir /s /q build & if exist $(TARGET) del /q $(TARGET)
    RUN_CMD   := $(TARGET)
else
    FLEX      := flex
    BISON     := bison
    TARGET    := compiler
    MKDIR_CMD := mkdir -p build
    CLEAN_CMD := rm -rf build $(TARGET)
    RUN_CMD   := ./$(TARGET)
endif

SRC_DIR   := src
BUILD_DIR := build
LEX_FILE  := $(SRC_DIR)/lexer.l
YACC_FILE := $(SRC_DIR)/parser.y

LEX_GEN    := $(BUILD_DIR)/lex.yy.c
YACC_GEN_C := $(BUILD_DIR)/y.tab.c
YACC_GEN_H := $(BUILD_DIR)/y.tab.h

.PHONY: all build clean run

all: $(TARGET)

build: all

$(YACC_GEN_C) $(YACC_GEN_H): $(YACC_FILE)
	@$(MKDIR_CMD)
	$(BISON) -d -o $(YACC_GEN_C) $(YACC_FILE)

$(LEX_GEN): $(LEX_FILE) $(YACC_GEN_H)
	@$(MKDIR_CMD)
	$(FLEX) -o $(LEX_GEN) $(LEX_FILE)

$(TARGET): $(YACC_GEN_C) $(LEX_GEN)
	$(CXX) $(CXXFLAGS) -x c++ $(YACC_GEN_C) $(LEX_GEN) -o $(TARGET)

clean:
	@$(CLEAN_CMD)

run: $(TARGET)
	$(RUN_CMD) $(FILE)