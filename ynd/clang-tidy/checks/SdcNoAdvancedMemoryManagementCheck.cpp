#include "SdcNoAdvancedMemoryManagementCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoAdvancedMemoryManagementCheck::SdcNoAdvancedMemoryManagementCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoAdvancedMemoryManagementCheck::registerMatchers(MatchFinder* Finder) {
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

                // Match calls to banned memory functions
                Finder->addMatcher(
                    callExpr(
                        callee(functionDecl()),
                        unless(isExpansionInSystemHeader()))
                        .bind("function_call"),
                    this);
            }

            void SdcNoAdvancedMemoryManagementCheck::check(
                const MatchFinder::MatchResult& Result) {
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

                // Check for calls to banned memory functions
                if (const auto* Call = Result.Nodes.getNodeAs<CallExpr>("function_call")) {
                    const auto* Callee = Call->getDirectCallee();
                    if (Callee && isBannedMemoryFunction(Callee)) {
                        diag(Call->getBeginLoc(),
                             "function '%0' is not allowed; "
                             "advanced memory management shall not be used")
                            << Callee->getName();
                        return;
                    }
                }
            }

            bool SdcNoAdvancedMemoryManagementCheck::isBannedMemoryFunction(
                const FunctionDecl* FD) {
                if (!FD) {
                    return false;
                }

                // Skip if it's not a simple function name (e.g., operators, destructors)
                if (!FD->getDeclName().isIdentifier()) {
                    return false;
                }

                StringRef Name = FD->getName();

                // Check for banned functions from <memory>
                static const char* BannedFunctions[] = {
                    "launder",
                    "uninitialized_default_construct",
                    "uninitialized_value_construct",
                    "uninitialized_copy",
                    "uninitialized_move",
                    "uninitialized_fill",
                    "uninitialized_default_construct_n",
                    "uninitialized_value_construct_n",
                    "uninitialized_copy_n",
                    "uninitialized_move_n",
                    "uninitialized_fill_n",
                    "destroy",
                    "destroy_at",
                    "destroy_n"};

                for (const auto* BannedName : BannedFunctions) {
                    if (Name == BannedName) {
                        // Verify it's in the std namespace
                        if (const auto* NS = dyn_cast_or_null<NamespaceDecl>(FD->getDeclContext())) {
                            if (NS->getName() == "std") {
                                return true;
                            }
                        }
                    }
                }

                return false;
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
