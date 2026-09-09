#include "SdcDeclarationUtils.h"
#include "SdcCodeSelection.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Analysis/Analyses/ExprMutationAnalyzer.h"

namespace clang::tidy::sdc::declaration_detail {
bool inHeader(SourceLocation L, const SourceManager &SM) {
    L = getUltimateWrittenLocation(L, SM);
    return L.isValid() && SM.getFileID(L) != SM.getMainFileID();
}
bool namespaceScope(const DeclContext *DC) {
    return DC->isTranslationUnit() || DC->isNamespace();
}
bool classType(QualType T, ASTContext &C) {
    return C.getBaseElementType(T)->isRecordType();
}
}
