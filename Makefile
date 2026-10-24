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
IR_DIR      := ir
AST_DIR     := typed_ast

CORE_SRC := src/ast.cpp src/types.cpp src/semantic.cpp src/ir.cpp \
            src/ir_generator.cpp src/main.cpp
CORE_HDR := src/ast.h src/types.h src/semantic.h src/frontend.h \
            src/diagnostic.h src/ir.h src/ir_generator.h

.PHONY: all build outputs generate generate-ir generate-ast test run clean distclean regen conflicts

# Default: build only the compiler binary. Run ./run.sh separately to generate IR and typed AST.
all: $(TARGET)
build: $(TARGET)

$(BUILD_STAMP):
	mkdir -p $(BUILD_DIR)
	touch $@

# Bison creates both the parser implementation and its header.
$(PARSER_GEN) $(PARSER_HDR) &: $(PARSER_FILE) | $(BUILD_STAMP)
	@command -v $(BISON) >/dev/null 2>&1 || { echo "Error: Bison is required. Install it before building." >&2; exit 1; }
	$(BISON) -d --defines=$(PARSER_HDR) -o $(PARSER_GEN) $(PARSER_FILE)

# Flex needs the parser header, so the parser is generated first.
$(LEX_GEN): $(LEX_FILE) $(PARSER_HDR) | $(BUILD_STAMP)
	@command -v $(FLEX) >/dev/null 2>&1 || { echo "Error: Flex is required. Install it before building." >&2; exit 1; }
	$(FLEX) -o $@ $(LEX_FILE)

# Compile generated lexer/parser plus the handwritten C++ sources.
$(TARGET): $(LEX_GEN) $(PARSER_GEN) $(PARSER_HDR) $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) $(LEX_GEN) $(PARSER_GEN) $(CORE_SRC) -o $@

outputs generate test: $(TARGET)
	bash ./run.sh generate

generate-ir: $(TARGET)
	bash ./run.sh ir

generate-ast: $(TARGET)
	bash ./run.sh ast

run: $(TARGET)
	@test -n "$(FILE)" || { echo 'Usage: make run FILE=test/01_arithmetic.min' >&2; exit 2; }
	./$(TARGET) "$(FILE)"

regen:
	rm -rf $(BUILD_DIR) $(TARGET)
	$(MAKE) $(TARGET)

conflicts: | $(BUILD_STAMP)
	@command -v $(BISON) >/dev/null 2>&1 || { echo "Error: Bison is required." >&2; exit 1; }
	$(BISON) -d --defines=$(BUILD_DIR)/parser.conflicts.h -o $(BUILD_DIR)/parser.conflicts.cpp $(PARSER_FILE) -Wcounterexamples --report=solved

clean:
	rm -f $(TARGET) minerva_asan
	rm -rf $(BUILD_DIR) $(IR_DIR) $(AST_DIR) generated

distclean: clean
