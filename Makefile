
CXX       ?= g++
CXXFLAGS  ?= -Wall -Wno-unused-function -std=c++17 -Isrc -Ibuild
FLEX      ?= flex
BISON     ?= bison

TARGET      := minerva
BUILD_DIR   := build
BUILD_STAMP := $(BUILD_DIR)/.dir-stamp
LEX_FILE    := src/lexer.l
PARSER_FILE := src/parser.y
LEX_GEN     := $(BUILD_DIR)/lex.yy.cpp
PARSER_GEN  := $(BUILD_DIR)/parser.tab.cpp
PARSER_HDR  := $(BUILD_DIR)/parser.tab.h

CORE_SRC := src/ast.cpp src/types.cpp src/semantic.cpp src/ir.cpp \
            src/ir_generator.cpp src/main.cpp
CORE_HDR := src/ast.h src/types.h src/semantic.h src/frontend.h \
            src/diagnostic.h src/ir.h src/ir_generator.h

.PHONY: all minerva clean regen

# Default command: build the compiler only.
all: $(TARGET)

minerva: $(TARGET)

$(BUILD_STAMP):
	mkdir -p $(BUILD_DIR)
	touch $@

$(PARSER_GEN) $(PARSER_HDR) &: $(PARSER_FILE) | $(BUILD_STAMP)
	@command -v $(BISON) >/dev/null 2>&1 || { echo "Error: Bison is required. Install with: sudo apt install bison" >&2; exit 1; }
	$(BISON) -d --defines=$(PARSER_HDR) -o $(PARSER_GEN) $(PARSER_FILE)

$(LEX_GEN): $(LEX_FILE) $(PARSER_HDR) | $(BUILD_STAMP)
	@command -v $(FLEX) >/dev/null 2>&1 || { echo "Error: Flex is required. Install with: sudo apt install flex" >&2; exit 1; }
	$(FLEX) -o $@ $(LEX_FILE)

$(TARGET): $(LEX_GEN) $(PARSER_GEN) $(PARSER_HDR) $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) $(LEX_GEN) $(PARSER_GEN) $(CORE_SRC) -o $@

# Remove compiler and generated output.
clean:
	rm -f $(TARGET) minerva_asan
	rm -rf $(BUILD_DIR) ir typed_ast generated

# Regenerate lexer and parser, then build the compiler.
regen:
	rm -rf $(BUILD_DIR) $(TARGET)
	$(MAKE) $(TARGET)
