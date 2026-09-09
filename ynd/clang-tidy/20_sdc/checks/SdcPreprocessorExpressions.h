#pragma once
#include "bridge_header.h"
namespace clang::tidy::sdc {
void registerExpressionPreprocessing(ClangTidyCheck &Check, Preprocessor &PP, bool Parentheses);
}
