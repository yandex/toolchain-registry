#include "SdcNoAdvancedMemoryManagementCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

            bool hasQualifiedRecordName(QualType Type, StringRef Name) {
                Type = Type.getUnqualifiedType();
                if (const auto* RT = Type->getAs<RecordType>())
                    return RT->getDecl()->getQualifiedNameAsString() == Name;
                return false;
            }

            bool isVoidPointer(QualType Type, const ASTContext& Context) {
                Type = Type.getUnqualifiedType();
                const auto* Pointer = Type->getAs<PointerType>();
                return Pointer &&
                       Context.hasSameType(
                           Pointer->getPointeeType().getUnqualifiedType(),
                           Context.VoidTy);
            }

            bool isConstNothrowReference(QualType Type) {
                const auto* Reference = Type->getAs<LValueReferenceType>();
                if (!Reference)
                    return false;
                const QualType Pointee = Reference->getPointeeType();
                return Pointee.isConstQualified() &&
                       !Pointee.isVolatileQualified() &&
                       hasQualifiedRecordName(Pointee, "std::nothrow_t");
            }

            bool hasExactAllowedSignature(const FunctionDecl* FD,
                                          const ASTContext& Context) {
                if (!FD) return false;
                DeclarationName Name = FD->getDeclName();
                if (Name.getNameKind() != DeclarationName::CXXOperatorName)
                    return false;
                OverloadedOperatorKind Op = Name.getCXXOverloadedOperator();
                if (Op != OO_New && Op != OO_Array_New &&
                    Op != OO_Delete && Op != OO_Array_Delete)
                    return false;

                if (Op == OO_New || Op == OO_Array_New) {
                    if (!isVoidPointer(FD->getReturnType(), Context) ||
                        (FD->getNumParams() != 1 &&
                         FD->getNumParams() != 2) ||
                        !Context.hasSameType(
                            FD->getParamDecl(0)->getType().getUnqualifiedType(),
                            Context.getSizeType())) {
                        return false;
                    }
                    return FD->getNumParams() == 1 ||
                           isConstNothrowReference(
                               FD->getParamDecl(1)->getType());
                }

                if (!Context.hasSameType(
                        FD->getReturnType().getUnqualifiedType(),
                        Context.VoidTy) ||
                    (FD->getNumParams() != 1 && FD->getNumParams() != 2) ||
                    !isVoidPointer(FD->getParamDecl(0)->getType(), Context)) {
                    return false;
                }

                const auto* FunctionType =
                    FD->getType()->getAs<FunctionProtoType>();
                if (!FunctionType ||
                    !FunctionType->isNothrow(
                        /*ResultIfDependent=*/false)) {
                    return false;
                }

                if (FD->getNumParams() == 1)
                    return true;

                const QualType Second = FD->getParamDecl(1)->getType();
                return Context.hasSameType(Second.getUnqualifiedType(),
                                           Context.getSizeType()) ||
                       isConstNothrowReference(Second);
            }

            bool isAdvancedOperator(const FunctionDecl* FD,
                                    const ASTContext& Context) {
                return FD && !hasExactAllowedSignature(FD, Context);
            }

            } // namespace

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

                // Match user-declared operator new/delete. Applicability is
                // decided from the declaration token's ultimate source origin
                // in check(), including macro-body origin.
                Finder->addMatcher(
                    functionDecl(
                        anyOf(hasName("operator new"), hasName("operator new[]"),
                              hasName("operator delete"), hasName("operator delete[]")))
                        .bind("user_operator_new_delete"),
                    this);

                // Calls to placement/custom allocation functions are encoded
                // as CXXNewExpr rather than ordinary CallExpr nodes.
                Finder->addMatcher(
                    cxxNewExpr(unless(isExpansionInSystemHeader()))
                        .bind("advanced_new_expression"),
                    this);

                Finder->addMatcher(
                    cxxDeleteExpr(unless(isExpansionInSystemHeader()))
                        .bind("advanced_delete_expression"),
                    this);

                // Taking the address of an overloaded operator resolves to a
                // DeclRefExpr once the target function-pointer type selects an
                // overload.
                Finder->addMatcher(
                    declRefExpr(
                        to(functionDecl(anyOf(
                            hasOverloadedOperatorName("new"),
                            hasOverloadedOperatorName("new[]"),
                            hasOverloadedOperatorName("delete"),
                            hasOverloadedOperatorName("delete[]")))),
                        unless(isExpansionInSystemHeader()))
                        .bind("advanced_operator_reference"),
                    this);
            }

            void SdcNoAdvancedMemoryManagementCheck::check(
                const MatchFinder::MatchResult& Result) {
                // Rules 1 & 2 for the <memory> function list: the base class
                // returns immediately when its bindings aren't present, so this
                // is safe to call for every match.
                SdcProhibitedFunctionsCheck::check(Result);

                if (const auto* New =
                        Result.Nodes.getNodeAs<CXXNewExpr>(
                            "advanced_new_expression")) {
                    const SourceLocation Loc = New->getBeginLoc();
                    if (isAdvancedOperator(New->getOperatorNew(),
                                           *Result.Context)) {
                        for (const Decl* Instance : AnalysisInstances.claim(
                                 *New, Loc, *Result.Context)) {
                            (void)Instance;
                            diag(New->getBeginLoc(),
                                 "call to an advanced allocation function "
                                 "is not allowed; advanced memory management "
                                 "shall not be used");
                        }
                    }
                    return;
                }

                if (const auto* Delete =
                        Result.Nodes.getNodeAs<CXXDeleteExpr>(
                            "advanced_delete_expression")) {
                    const SourceLocation Loc = Delete->getBeginLoc();
                    if (isAdvancedOperator(Delete->getOperatorDelete(),
                                           *Result.Context)) {
                        for (const Decl* Instance : AnalysisInstances.claim(
                                 *Delete, Loc, *Result.Context)) {
                            (void)Instance;
                            diag(Loc,
                                 "call to an advanced deallocation function is "
                                 "not allowed; advanced memory management shall "
                                 "not be used");
                        }
                    }
                    return;
                }

                if (const auto* Ref =
                        Result.Nodes.getNodeAs<DeclRefExpr>(
                            "advanced_operator_reference")) {
                    const auto* FD = dyn_cast<FunctionDecl>(Ref->getDecl());
                    const SourceLocation Loc = Ref->getBeginLoc();
                    if (isAdvancedOperator(FD, *Result.Context)) {
                        for (const Decl* Instance : AnalysisInstances.claim(
                                 *Ref, Loc, *Result.Context)) {
                            (void)Instance;
                            diag(Ref->getBeginLoc(),
                                 "use of an advanced allocation or deallocation "
                                 "function is not allowed; advanced memory "
                                 "management shall not be used");
                        }
                    }
                    return;
                }

                // Check for explicit destructor calls
                if (const auto* DestructorCall = Result.Nodes.getNodeAs<CXXMemberCallExpr>("explicit_destructor")) {
                    // Check if this is an explicit destructor call
                    // Implicit destructor calls won't match this pattern in the AST
                    const SourceLocation Loc = DestructorCall->getExprLoc();
                    if (Loc.isValid()) {
                        for (const Decl* Instance : AnalysisInstances.claim(
                                 *DestructorCall, Loc, *Result.Context)) {
                            (void)Instance;
                            diag(DestructorCall->getExprLoc(),
                                 "explicit destructor call is not allowed; "
                                 "advanced memory management shall not be used");
                        }
                    }
                    return;
                }

                // Check for user-declared operator new/delete
                if (const auto* OpFunc = Result.Nodes.getNodeAs<FunctionDecl>("user_operator_new_delete")) {
                    if (isUserDeclaredOperatorNewDelete(OpFunc)) {
                        SourceManager& SM = Result.Context->getSourceManager();

                        // Try multiple ways to get a valid source location
                        // The declaration can place its return type on a
                        // preceding line. Point at `operator new/delete`, the
                        // token the developer needs to remove or redesign.
                        SourceLocation Loc = OpFunc->getLocation();
                        if (!Loc.isValid()) {
                            Loc = OpFunc->getBeginLoc();
                        }
                        if (!Loc.isValid() && OpFunc->hasBody()) {
                            Loc = OpFunc->getBody()->getBeginLoc();
                        }

                        const auto Instances = AnalysisInstances.claim(
                            *OpFunc, Loc, *Result.Context);
                        if (Instances.empty()) {
                            return;
                        }

                        // Get the presumed location which handles #line directives correctly
                        PresumedLoc PLoc = SM.getPresumedLoc(Loc);

                        for (const Decl* Instance : Instances) {
                            (void)Instance;
                            if (PLoc.isValid()) {
                                diag(Loc,
                                     "user-declared %0 is not allowed; "
                                     "advanced memory management shall not be "
                                     "used [in %1:%2]")
                                    << OpFunc->getDeclName()
                                    << PLoc.getFilename() << PLoc.getLine();
                            } else if (Loc.isValid()) {
                                StringRef Filename = SM.getFilename(Loc);
                                diag(Loc,
                                     "user-declared %0 is not allowed; "
                                     "advanced memory management shall not be "
                                     "used [in %1]")
                                    << OpFunc->getDeclName()
                                    << (Filename.empty() ? "<unknown>"
                                                         : Filename);
                            } else {
                                FileID MainFileID = SM.getMainFileID();
                                Loc = SM.getLocForStartOfFile(MainFileID);
                                diag(Loc,
                                     "user-declared %0 is not allowed; "
                                     "advanced memory management shall not be "
                                     "used [location unavailable]")
                                    << OpFunc->getDeclName();
                            }
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
