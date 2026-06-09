#include "SdcMacroMixedUseCheck.h"
#include "SdcPreprocessorUtils.h"

#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

namespace clang {
namespace tidy {
namespace sdc {

SdcMacroMixedUseCheck::SdcMacroMixedUseCheck(StringRef Name,
                                               ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

// Returns true if any token in the unexpanded argument is itself an
// expandable macro (i.e., the argument is "subject to further expansion").
static bool argContainsMacro(const Token* ArgToks, Preprocessor& PP) {
    for (const Token* T = ArgToks; !T->is(tok::eof); ++T) {
        if (!T->is(tok::identifier)) continue;
        const IdentifierInfo* II = T->getIdentifierInfo();
        if (II && PP.getMacroInfo(const_cast<IdentifierInfo*>(II)))
            return true;
    }
    return false;
}

struct MixedParamInfo {
    llvm::SmallVector<bool, 8> IsMixed; // IsMixed[i] = true iff param i is mixed
};

class MixedUseCallbacks : public PPCallbacks {
    SdcMacroMixedUseCheck& Check;
    const SourceManager& SM;
    Preprocessor& PP;

    // Keyed by the macro name's IdentifierInfo* for stability across callbacks.
    llvm::DenseMap<const IdentifierInfo*, MixedParamInfo> MixedMacros;

public:
    MixedUseCallbacks(SdcMacroMixedUseCheck& C, const SourceManager& SM,
                       Preprocessor& PP)
        : Check(C), SM(SM), PP(PP) {}

    void MacroDefined(const Token& MacroNameTok,
                      const MacroDirective* MD) override {
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;

        const MacroInfo* MI = MD->getMacroInfo();
        if (!MI || !MI->isFunctionLike() || MI->getNumParams() == 0) return;

        auto Usage = ::sdc::pp::classifyParamUsage(MI);
        MixedParamInfo Info;
        Info.IsMixed.resize(MI->getNumParams(), false);
        bool HasMixed = false;

        for (unsigned I = 0; I < MI->getNumParams(); ++I) {
            if (::sdc::pp::isMixedUse(Usage[I])) {
                Info.IsMixed[I] = true;
                HasMixed = true;
            }
        }

        if (HasMixed)
            MixedMacros[MacroNameTok.getIdentifierInfo()] = std::move(Info);
    }

    void MacroExpands(const Token& MacroNameTok, const MacroDefinition& MD,
                      SourceRange /*Range*/,
                      const MacroArgs* Args) override {
        if (::sdc::pp::isSystemHeader(MacroNameTok.getLocation(), SM)) return;
        if (!Args) return;

        const IdentifierInfo* Name = MacroNameTok.getIdentifierInfo();
        auto It = MixedMacros.find(Name);
        if (It == MixedMacros.end()) return;

        const MacroInfo* MI = MD.getMacroInfo();
        if (!MI) return;

        const auto& Mixed = It->second.IsMixed;

        for (unsigned I = 0;
             I < MI->getNumParams() && I < Mixed.size(); ++I) {
            if (!Mixed[I]) continue;

            const Token* ArgToks = Args->getUnexpArgument(I);
            if (!ArgToks || ArgToks->is(tok::eof)) continue;

            if (argContainsMacro(ArgToks, PP)) {
                // Get parameter name for the diagnostic.
                StringRef ParamName;
                auto PIt = MI->param_begin();
                std::advance(PIt, I);
                if (PIt != MI->param_end())
                    ParamName = (*PIt)->getName();

                Check.diag(MacroNameTok.getLocation(),
                            "argument for mixed-use parameter '%0' of macro "
                            "'%1' is subject to further macro expansion")
                    << ParamName
                    << MacroNameTok.getIdentifierInfo()->getName();
            }
        }
    }
};

} // namespace

void SdcMacroMixedUseCheck::registerPPCallbacks(const SourceManager& SM,
                                                   Preprocessor* PP,
                                                   Preprocessor* /*MEP*/) {
    PP->addPPCallbacks(
        std::make_unique<MixedUseCallbacks>(*this, SM, *PP));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
