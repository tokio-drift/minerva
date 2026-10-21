SRC_DIR   := src
BUILD_DIR := build
CXX      := g++
CXXFLAGS := -Wall -Wno-unused-function -std=c++17 -I$(SRC_DIR) -I$(BUILD_DIR)
FLEX     := flex
BISON    := bison

LEX_FILE  := $(SRC_DIR)/lexer.l
PARSER_FILE := $(SRC_DIR)/parser.y
LEX_GEN   := $(BUILD_DIR)/lex.yy.cpp
PARSER_GEN := $(BUILD_DIR)/parser.tab.cpp
PARSER_HDR := $(BUILD_DIR)/parser.tab.h
TARGET    := minerva
CORE_SRC  := $(SRC_DIR)/ast.cpp $(SRC_DIR)/types.cpp $(SRC_DIR)/semantic.cpp $(SRC_DIR)/main.cpp
CORE_HDR  := $(SRC_DIR)/ast.h $(SRC_DIR)/types.h $(SRC_DIR)/semantic.h $(SRC_DIR)/frontend.h

.PHONY: all clean run conflicts summ_conflicts asan test test-ast test-semantic

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

$(TARGET): $(LEX_GEN) $(PARSER_GEN) $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) $(LEX_GEN) $(PARSER_GEN) $(CORE_SRC) -o $(TARGET) -lfl

# Sanitizer build (AddressSanitizer + UBSan); output: ./minerva_asan
asan: $(LEX_GEN) $(PARSER_GEN) $(CORE_SRC) $(CORE_HDR)
	$(CXX) $(CXXFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer $(LEX_GEN) $(PARSER_GEN) $(CORE_SRC) -o minerva_asan

# Golden AST tests (Phase 1)
test-ast: $(TARGET)
	bash test/ast_tests.sh ./$(TARGET)

# Semantic analysis tests (Phase 2)
test-semantic: $(TARGET)
	bash test/semantic_tests.sh ./$(TARGET)

test: test-ast test-semantic

clean:
	rm -rf $(BUILD_DIR) $(TARGET) minerva_asan

run: $(TARGET)
	./$(TARGET) $(FILE)

conflicts:
	mkdir -p $(BUILD_DIR)
	$(BISON) -d --defines=$(BUILD_DIR)/parser.conflicts.h -o $(BUILD_DIR)/parser.conflicts.cpp $(PARSER_FILE) -Wcounterexamples --report=solved

summ_conflicts: 
	python3 scripts/summarize_conflicts.py
