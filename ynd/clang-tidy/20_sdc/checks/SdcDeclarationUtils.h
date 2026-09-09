#pragma once
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
namespace clang::tidy::sdc::declaration_detail {
bool inHeader(SourceLocation, const SourceManager &);
bool namespaceScope(const DeclContext *);
bool classType(QualType, ASTContext &);
}
