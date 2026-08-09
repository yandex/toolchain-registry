#include "SdcNoSystemCheck.h"

#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/Token.h"

namespace clang {
    namespace tidy {
        namespace sdc {

            namespace {

                class SystemMacroCallbacks : public PPCallbacks {
                public:
                    SystemMacroCallbacks(SdcNoSystemCheck& Check,
                                         const SourceManager& SM)
                        : Check_(Check), SM_(SM) {}

                    void MacroExpands(const Token& MacroNameTok,
                                      const MacroDefinition&,
                                      SourceRange,
                                      const MacroArgs*) override {
                        const IdentifierInfo* II =
                            MacroNameTok.getIdentifierInfo();
                        if (!II || II->getName() != "system") return;

                        SourceLocation Loc = MacroNameTok.getLocation();
                        if (Loc.isInvalid() || SM_.isInSystemHeader(Loc)) return;
                        Check_.diag(Loc,
                                    "macro named 'system' shall not be expanded");
                    }

                private:
                    SdcNoSystemCheck& Check_;
                    const SourceManager& SM_;
                };

            } // namespace

            static const StringRef ProhibitedSystemFunctions[] = {
                "::system",
                "::std::system",
                // Project/QM extension: this launcher has the same shell and
                // environment-dependent command-resolution risks even though
                // it is not the prohibited C library function.
                "::boost::process::system"
            };

            SdcNoSystemCheck::SdcNoSystemCheck(
                StringRef Name, ClangTidyContext* Context)
                : SdcProhibitedFunctionsCheck(Name, Context)
            {
            }

            void SdcNoSystemCheck::registerPPCallbacks(
                const SourceManager& SM, Preprocessor* PP,
                Preprocessor* /*ModuleExpanderPP*/) {
                PP->addPPCallbacks(
                    std::make_unique<SystemMacroCallbacks>(*this, SM));
            }

            ArrayRef<StringRef> SdcNoSystemCheck::getProhibitedFunctions() const {
                return ProhibitedSystemFunctions;
            }

            std::string SdcNoSystemCheck::getDiagnosticMessage(StringRef FunctionName) const {
                return "shell-command launcher 'system' shall not be used";
            }

        } // namespace sdc
    } // namespace tidy
} // namespace clang
