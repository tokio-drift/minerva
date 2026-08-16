# Minerva

This repository contains the code for Minerva, a compiler for a C-like programming language, built using **C++** and **Flex**.

---

## Project Structure

* `src/`: Contains the source code and implementation for the lexical analyzer.
* `test/`: Contains individual test files (e.g., test cases for operators, keywords, constants, identifiers and other tokens).
* `Makefile`: Automates building, running, and cleaning the project.
* `run.sh`: Bash script which could be used to run the complete test suite.

---

## Prerequisites

Before getting started, make sure you have **Flex** installed on your system. If you don't have it installed, you can install it using your system's package manager:

* **Ubuntu/Debian:** `sudo apt-get install flex`
* **macOS (Homebrew):** `brew install flex`

---

## Steps to run the analyzer

1. **Clone the repository:**
   ```bash
   git clone <repository-url>
   cd minerva
    ```

2. **Run the test cases:**
    ```bash
    make build
    ```
    To run the complete test suite, use the command:
    ```bash
    ./run.sh
    ```

    To run individual test cases, you can use the command:
    ```bash
    ./lexer /test/test1_operators.min OR make run FILE=/path/to/file
    ```

3. **Clean up the build files:**
    ```bash
    make clean
    ```


