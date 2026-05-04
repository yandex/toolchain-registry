#include "SdcDynamicMemoryAutomaticCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcDynamicMemoryAutomaticCheck::SdcDynamicMemoryAutomaticCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcDynamicMemoryAutomaticCheck::registerMatchers(MatchFinder* Finder) {
                // Match new expressions (we'll filter placement new in check())
                Finder->addMatcher(
                    cxxNewExpr(
                        unless(isExpansionInSystemHeader()))
                        .bind("new_expr"),
                    this);

                // Match delete expressions
                Finder->addMatcher(
                    cxxDeleteExpr(
                        unless(isExpansionInSystemHeader()))
                        .bind("delete_expr"),
                    this);

                // Match calls to malloc, calloc, realloc, aligned_alloc, free
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(hasAnyName("::malloc", "::calloc", "::realloc",
                                                       "::aligned_alloc", "::free"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("malloc_family"),
                    this);

                // Match calls to allocate/deallocate methods in std namespace
                Finder->addMatcher(
                    cxxMemberCallExpr(
                        callee(cxxMethodDecl(hasAnyName("allocate", "deallocate"))),
                        unless(isExpansionInSystemHeader()))
                        .bind("std_allocate"),
                    this);

                // Match calls to std::unique_ptr::release
                Finder->addMatcher(
                    cxxMemberCallExpr(
                        callee(cxxMethodDecl(hasName("release"))),
                        on(hasType(hasUnqualifiedDesugaredType(
                            recordType(hasDeclaration(
                                classTemplateSpecializationDecl(
                                    hasName("::std::unique_ptr"))))))),
                        unless(isExpansionInSystemHeader()))
                        .bind("unique_ptr_release"),
                    this);

                // Match taking address of operator new/delete
                Finder->addMatcher(
                    unaryOperator(
                        hasOperatorName("&"),
                        hasUnaryOperand(declRefExpr(to(functionDecl(
                            anyOf(hasName("operator new"), hasName("operator delete"),
                                  hasName("operator new[]"), hasName("operator delete[]")))))))
                        .bind("address_of_new_delete"),
                    this);

                // Match explicit calls to operator new/delete
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl(
                            anyOf(hasName("operator new"), hasName("operator delete"),
                                  hasName("operator new[]"), hasName("operator delete[]")))),
                        unless(isExpansionInSystemHeader()))
                        .bind("operator_new_delete_call"),
                    this);
            }

            void SdcDynamicMemoryAutomaticCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Check for new expressions (but allow placement new)
                if (const auto* NewExpr = Result.Nodes.getNodeAs<CXXNewExpr>("new_expr")) {
                    // Placement new has placement arguments
                    // However, new (std::nothrow) is NOT placement new
                    // Real placement new takes a pointer to existing memory
                    if (NewExpr->getNumPlacementArgs() > 0) {
                        // Check if this is the nothrow form or actual placement new
                        // The nothrow form has a single argument of type std::nothrow_t
                        bool isNothrow = false;
                        if (NewExpr->getNumPlacementArgs() == 1) {
                            QualType ArgType = NewExpr->getPlacementArg(0)->getType();
                            if (const auto* RecType = ArgType->getAs<RecordType>()) {
                                if (const auto* RD = RecType->getDecl()) {
                                    if (RD->getName() == "nothrow_t") {
                                        isNothrow = true;
                                    }
                                }
                            }
                        }

                        // If it's nothrow, warn. Otherwise it's real placement new (allowed)
                        if (!isNothrow) {
                            return; // Real placement new is allowed
                        }
                    }
                    diag(NewExpr->getBeginLoc(),
                         "non-placement new is not allowed; "
                         "dynamic memory shall be managed automatically");
                    return;
                }

                // Check for delete expressions
                if (const auto* DeleteExpr = Result.Nodes.getNodeAs<CXXDeleteExpr>("delete_expr")) {
                    diag(DeleteExpr->getBeginLoc(),
                         "delete is not allowed; "
                         "dynamic memory shall be managed automatically");
                    return;
                }

                // Check for malloc family functions
                if (const auto* MallocCall = Result.Nodes.getNodeAs<CallExpr>("malloc_family")) {
                    const auto* Callee = MallocCall->getDirectCallee();
                    if (Callee) {
                        diag(MallocCall->getBeginLoc(),
                             "function '%0' is not allowed; "
                             "dynamic memory shall be managed automatically")
                            << Callee->getName();
                    }
                    return;
                }

                // Check for std::allocate/deallocate methods
                if (const auto* AllocateCall = Result.Nodes.getNodeAs<CXXMemberCallExpr>("std_allocate")) {
                    const auto* Method = AllocateCall->getMethodDecl();
                    if (Method && isInStdNamespace(Method)) {
                        diag(AllocateCall->getBeginLoc(),
                             "std::%0 is not allowed; "
                             "dynamic memory shall be managed automatically")
                            << Method->getName();
                    }
                    return;
                }

                // Check for std::unique_ptr::release
                if (const auto* ReleaseCall = Result.Nodes.getNodeAs<CXXMemberCallExpr>("unique_ptr_release")) {
                    diag(ReleaseCall->getBeginLoc(),
                         "std::unique_ptr::release is not allowed; "
                         "dynamic memory shall be managed automatically");
                    return;
                }

                // Check for taking address of operator new/delete
                if (const auto* AddrOf = Result.Nodes.getNodeAs<UnaryOperator>("address_of_new_delete")) {
                    diag(AddrOf->getBeginLoc(),
                         "taking address of operator new/delete is not allowed; "
                         "dynamic memory shall be managed automatically");
                    return;
                }

                // Check for explicit operator new/delete calls
                if (const auto* OpCall = Result.Nodes.getNodeAs<CallExpr>("operator_new_delete_call")) {
                    const auto* Callee = OpCall->getDirectCallee();
                    if (Callee) {
                        diag(OpCall->getBeginLoc(),
                             "explicit call to operator new/delete is not allowed; "
                             "dynamic memory shall be managed automatically");
                    }
                    return;
                }
            }

            bool SdcDynamicMemoryAutomaticCheck::isInStdNamespace(
                const clang::CXXMethodDecl* Method) {
                if (!Method) {
                    return false;
                }

                const DeclContext* DC = Method->getDeclContext();
                while (DC) {
                    if (const auto* NS = dyn_cast<NamespaceDecl>(DC)) {
                        if (NS->getName() == "std") {
                            return true;
                        }
                    }
                    DC = DC->getParent();
                }

                return false;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
