#include "SdcNoGlobalVariablesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang {
    namespace tidy {
        namespace sdc {

            SdcNoGlobalVariablesCheck::SdcNoGlobalVariablesCheck(
                StringRef Name, ClangTidyContext* Context)
                : ClangTidyCheck(Name, Context)
            {
            }

            void SdcNoGlobalVariablesCheck::registerMatchers(MatchFinder* Finder) {
                // We match every VarDecl and filter in check() so that we can
                // distinguish namespace-scope variables, class static data
                // members, and local variables - and so that we can reach the
                // VarDecl's enclosing context to detect template instantiations.
                Finder->addMatcher(
                    varDecl(unless(isExpansionInSystemHeader())).bind("var"),
                    this);
                // varDecl() alone does not visit the inner VarDecl of a
                // VarTemplateDecl in clang-20's matcher set (no
                // varTemplateDecl() matcher is provided), so use a generic
                // decl() matcher and dyn_cast in check().
                Finder->addMatcher(
                    decl(unless(isExpansionInSystemHeader())).bind("vtd"),
                    this);
            }

            namespace {

                // Returns true if VD lives at namespace scope (including the
                // translation-unit scope and anonymous namespaces).
                bool isNamespaceScopeVar(const VarDecl* VD) {
                    const DeclContext* DC = VD->getDeclContext();
                    if (!DC) {
                        return false;
                    }
                    // Skip locals, parameters, and members.
                    if (DC->isFunctionOrMethod()) {
                        return false;
                    }
                    if (VD->isStaticDataMember()) {
                        return false;
                    }
                    // Walk through linkage specifications (extern "C" { ... }).
                    while (DC && isa<LinkageSpecDecl>(DC)) {
                        DC = DC->getParent();
                    }
                    if (!DC) {
                        return false;
                    }
                    return DC->isTranslationUnit() || DC->isNamespace();
                }

                // True if VD is itself an instantiation of a templated entity,
                // or if it is a static data member of a class-template
                // instantiation. We report only at the template pattern so
                // every instantiation does not re-trigger the diagnostic.
                bool isFromTemplateInstantiation(const VarDecl* VD) {
                    // For the templated VarDecl inside a class/variable
                    // template pattern, TSK_Undeclared is reported and we want
                    // to diagnose it. Implicit / explicit instantiations carry
                    // a non-Undeclared TSK and should be skipped (the pattern
                    // is visited separately).
                    TemplateSpecializationKind TSK =
                        VD->getTemplateSpecializationKind();
                    if (TSK == TSK_ImplicitInstantiation ||
                        TSK == TSK_ExplicitInstantiationDeclaration ||
                        TSK == TSK_ExplicitInstantiationDefinition ||
                        TSK == TSK_ExplicitSpecialization) {
                        return true;
                    }
                    if (VD->isStaticDataMember()) {
                        const auto* RD =
                            dyn_cast<CXXRecordDecl>(VD->getDeclContext());
                        if (RD) {
                            TemplateSpecializationKind RTSK =
                                RD->getTemplateSpecializationKind();
                            if (RTSK == TSK_ImplicitInstantiation ||
                                RTSK == TSK_ExplicitInstantiationDeclaration ||
                                RTSK == TSK_ExplicitInstantiationDefinition) {
                                return true;
                            }
                        }
                    }
                    return false;
                }

                // True if the variable's type is const-qualified.
                bool isConstQualified(const VarDecl* VD) {
                    return VD->getType().isConstQualified();
                }

                // True if the variable definition has constant (static)
                // initialization. Only called for actual definitions, never
                // for extern declarations (those are skipped earlier).
                bool hasStaticInitialization(const VarDecl* VD) {
                    if (!VD->hasInit()) {
                        return false;
                    }
                    return VD->hasConstantInitialization();
                }

            } // namespace

            void SdcNoGlobalVariablesCheck::check(
                const MatchFinder::MatchResult& Result) {
                const VarDecl* VD = Result.Nodes.getNodeAs<VarDecl>("var");
                if (!VD) {
                    if (const auto* D = Result.Nodes.getNodeAs<Decl>("vtd")) {
                        if (const auto* VTD = dyn_cast<VarTemplateDecl>(D)) {
                            VD = VTD->getTemplatedDecl();
                        } else {
                            return;
                        }
                    }
                }
                if (!VD) {
                    return;
                }

                const bool IsNamespaceScope = isNamespaceScopeVar(VD);
                const bool IsStaticMember = VD->isStaticDataMember();
                if (!IsNamespaceScope && !IsStaticMember) {
                    return;
                }

                if (isFromTemplateInstantiation(VD)) {
                    return;
                }

                // Skip `extern const T x;` declarations (not definitions).
                // The const+static-init exemption can only be evaluated at
                // the definition, where the initializer is present. The
                // definition will be checked in its own translation unit.
                // Non-const extern declarations are still flagged here because
                // they are definitively violations regardless of where the
                // definition lives.
                if (VD->isThisDeclarationADefinition() ==
                        VarDecl::DeclarationOnly &&
                    VD->hasExternalStorage() && isConstQualified(VD)) {
                    return;
                }

                if (VD->isConstexpr()) {
                    return;
                }

                if (isConstQualified(VD) && hasStaticInitialization(VD)) {
                    return;
                }

                diag(VD->getLocation(),
                     "global variable is not allowed; declare it as constexpr, "
                     "or as const with constant initialization, or move it to "
                     "function scope");
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
