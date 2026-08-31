#!/usr/bin/env bash
EXE="${1:-./minerva}"
pass=0
fail=0
GREEN="\033[1;32m"
RED="\033[1;31m"
CYAN="\033[1;36m"
ENDCLR="\033[0m"

for f in test/*.min; do
    echo "Running: $f"
    "$EXE" "$f"
    EXIT_CODE=$?
    if [[ "$f" == *"lexical_errors"* || "$f" == *"syntax_errors"* ]];then
        if [ "$EXIT_CODE" -eq 2 ]; then
            echo -e "Result: ${GREEN}PASS${ENDCLR}"
            ((pass++))
        else
            echo -e "Result: ${RED}FAIL${ENDCLR} (Expected errors)"
            ((fail++))
        fi
    else
        if [ "$EXIT_CODE" -eq 0 ]; then
            echo -e "Result: ${GREEN}PASS${ENDCLR}"
            ((pass++))
        else
            echo -e "Result: ${RED}FAIL${ENDCLR}"
            ((fail++))
        fi
    fi
    echo
done

echo -e "${CYAN}TEST RESULTS${ENDCLR}"
echo -e "${GREEN}Pass${ENDCLR}: ${pass}"
echo -e "${RED}Fail${ENDCLR}: ${fail}"