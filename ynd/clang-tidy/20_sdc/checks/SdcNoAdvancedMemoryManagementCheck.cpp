#include "SdcNoAdvancedMemoryManagementCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            // Rules 1 & 2 for the <memory> function list. The base class
            // matches both calls and address-of references against these names.
            static const StringRef ProhibitedAdvancedMemoryFunctions[] = {
                "::std::launder",
                "::std::uninitialized_default_construct",
                "::std::uninitialized_value_construct",
                "::std::uninitialized_copy",
                "::std::uninitialized_move",
                "::std::uninitialized_fill",
                "::std::uninitialized_default_construct_n",
                "::std::uninitialized_value_construct_n",
                "::std::uninitialized_copy_n",
                "::std::uninitialized_move_n",
                "::std::uninitialized_fill_n",
                "::std::destroy",
                "::std::destroy_at",
                "::std::destroy_n",
            };

            SdcNoAdvancedMemoryManagementCheck::SdcNoAdvancedMemoryManagementCheck(
                StringRef Name, ClangTidyContext* Context)
                : SdcProhibitedFunctionsCheck(Name, Context)
            {
            }

            ArrayRef<StringRef>
            SdcNoAdvancedMemoryManagementCheck::getProhibitedFunctions() const {
                return ProhibitedAdvancedMemoryFunctions;
            }

            std::string
            SdcNoAdvancedMemoryManagementCheck::getDiagnosticMessage(StringRef FunctionName) const {
                return "function 'std::" + FunctionName.str() + "' is not allowed; "
                       "advanced memory management shall not be used";
            }

            void SdcNoAdvancedMemoryManagementCheck::registerMatchers(MatchFinder* Finder) {
                // Rules 1 & 2 for the <memory> function list: delegate to base.
                SdcProhibitedFunctionsCheck::registerMatchers(Finder);

                // Match explicit destructor calls
                Finder->addMatcher(
                    cxxMemberCallExpr(
                        callee(cxxDestructorDecl()),
                        unless(isExpansionInSystemHeader()))
                        .bind("explicit_destructor"),
                    this);

                // Match user-declared operator new/delete
                // Note: We don't exclude system headers here because we want to catch
                // third-party libraries that declare these operators
                Finder->addMatcher(
                    functionDecl(
                        anyOf(hasName("operator new"), hasName("operator new[]"),
                              hasName("operator delete"), hasName("operator delete[]")))
                        .bind("user_operator_new_delete"),
                    this);
            }

            void SdcNoAdvancedMemoryManagementCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Rules 1 & 2 for the <memory> function list: the base class
                // returns immediately when its bindings aren't present, so this
                // is safe to call for every match.
                SdcProhibitedFunctionsCheck::check(Result);

                // Check for explicit destructor calls
                if (const auto* DestructorCall = Result.Nodes.getNodeAs<CXXMemberCallExpr>("explicit_destructor")) {
                    // Check if this is an explicit destructor call
                    // Implicit destructor calls won't match this pattern in the AST
                    if (DestructorCall->getExprLoc().isValid()) {
                        diag(DestructorCall->getExprLoc(),
                             "explicit destructor call is not allowed; "
                             "advanced memory management shall not be used");
                    }
                    return;
                }

                // Check for user-declared operator new/delete
                if (const auto* OpFunc = Result.Nodes.getNodeAs<FunctionDecl>("user_operator_new_delete")) {
                    if (isUserDeclaredOperatorNewDelete(OpFunc)) {
                        SourceManager& SM = Result.Context->getSourceManager();

                        // Try multiple ways to get a valid source location
                        SourceLocation Loc = OpFunc->getBeginLoc();
                        if (!Loc.isValid()) {
                            Loc = OpFunc->getLocation();
                        }
                        if (!Loc.isValid() && OpFunc->hasBody()) {
                            Loc = OpFunc->getBody()->getBeginLoc();
                        }

                        // If location is in a macro, get the spelling location
                        if (Loc.isValid() && Loc.isMacroID()) {
                            Loc = SM.getSpellingLoc(Loc);
                        }

                        // Skip if this is from the compiler's builtin definitions
                        if (Loc.isValid() && SM.isWrittenInBuiltinFile(Loc)) {
                            return;
                        }

                        // Get the presumed location which handles #line directives correctly
                        PresumedLoc PLoc = SM.getPresumedLoc(Loc);

                        // If we have a valid presumed location, use it
                        if (PLoc.isValid()) {
                            diag(Loc,
                                 "user-declared %0 is not allowed; "
                                 "advanced memory management shall not be used "
                                 "[in %1:%2]")
                                << OpFunc->getDeclName() << PLoc.getFilename() << PLoc.getLine();
                        } else if (Loc.isValid()) {
                            // Fallback: just use what we have
                            StringRef Filename = SM.getFilename(Loc);
                            diag(Loc,
                                 "user-declared %0 is not allowed; "
                                 "advanced memory management shall not be used "
                                 "[in %1]")
                                << OpFunc->getDeclName() << (Filename.empty() ? "<unknown>" : Filename);
                        } else {
                            // Last resort: no location at all, report at main file start
                            FileID MainFileID = SM.getMainFileID();
                            Loc = SM.getLocForStartOfFile(MainFileID);
                            diag(Loc,
                                 "user-declared %0 is not allowed; "
                                 "advanced memory management shall not be used "
                                 "[location unavailable]")
                                << OpFunc->getDeclName();
                        }
                        return;
                    }
                }

            }

            bool SdcNoAdvancedMemoryManagementCheck::isUserDeclaredOperatorNewDelete(
                const FunctionDecl* FD) {
                if (!FD) {
                    return false;
                }

                // Check if it's an operator new/delete
                DeclarationName DeclName = FD->getDeclName();
                if (DeclName.getNameKind() != DeclarationName::CXXOperatorName) {
                    return false;
                }

                OverloadedOperatorKind OpKind = DeclName.getCXXOverloadedOperator();
                if (OpKind != OO_New && OpKind != OO_Array_New &&
                    OpKind != OO_Delete && OpKind != OO_Array_Delete) {
                    return false;
                }

                // Skip implicit declarations (compiler-generated)
                if (FD->isImplicit()) {
                    return false;
                }

                // Skip if no valid source location (built-in/compiler intrinsic)
                SourceLocation Loc = FD->getLocation();
                if (!Loc.isValid()) {
                    return false;
                }

                // Skip builtin functions
                if (FD->getBuiltinID() != 0) {
                    return false;
                }

                // Skip `= delete`d declarations. The user is explicitly forbidding
                // the operator, which aligns with the rule rather than violating it
                // (e.g. `void* operator new[](size_t) = delete;` to ban heap allocation
                // of a class). Note: clang reports `isThisDeclarationADefinition()` as
                // true for deleted functions, so this must come before that check.
                if (FD->isDeleted()) {
                    return false;
                }

                // If it's user-declared (has a definition or is declared in user code), it's banned
                // Check if this is a definition (not just a declaration)
                if (FD->isThisDeclarationADefinition()) {
                    return true;
                }

                // Also ban explicit declarations (forward declarations of user operator new/delete)
                // But only if they have a valid location and aren't defaulted
                if (!FD->isDefaulted() && Loc.isValid()) {
                    return true;
                }

                return false;
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
