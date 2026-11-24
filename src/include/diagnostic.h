// diagnostic.h - diagnostic records shared by the semantic and IR stages
#pragma once
#include <string>

#include "ast.h"

struct Diagnostic {
    SrcLoc loc;
    std::string category;
    std::string message;
};
