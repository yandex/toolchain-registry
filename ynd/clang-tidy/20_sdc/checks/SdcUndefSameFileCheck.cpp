#include "SdcUndefSameFileCheck.h"

#include "clang/Basic/SourceManager.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"

#include <optional>
#include <set>
#include <utility>

namespace clang {
namespace tidy {
namespace sdc {

SdcUndefSameFileCheck::SdcUndefSameFileCheck(
    StringRef Name, ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

namespace {

using MacroKey = std::pair<unsigned, const IdentifierInfo*>;

class UndefSameFileCallbacks : public PPCallbacks {
    SdcUndefSameFileCheck& Check;
    const SourceManager& SM;
    std::set<MacroKey> LocallyDefinedMacros;

    std::optional<MacroKey> keyFor(const Token& MacroNameTok) const {
        const IdentifierInfo* Identifier = MacroNameTok.getIdentifierInfo();
        SourceLocation Location =
            SM.getSpellingLoc(MacroNameTok.getLocation());
        if (!Identifier || Location.isInvalid()) {
            return std::nullopt;
        }
        FileID File = SM.getFileID(Location);
        if (File.isInvalid()) {
            return std::nullopt;
        }
        return MacroKey(File.getHashValue(), Identifier);
    }

public:
    UndefSameFileCallbacks(SdcUndefSameFileCheck& Check,
                           const SourceManager& SM)
        : Check(Check), SM(SM) {}

    void MacroDefined(const Token& MacroNameTok,
                      const MacroDirective*) override {
        if (std::optional<MacroKey> Key = keyFor(MacroNameTok)) {
            LocallyDefinedMacros.insert(*Key);
        }
    }

    void MacroUndefined(const Token& MacroNameTok, const MacroDefinition&,
                        const MacroDirective*) override {
        std::optional<MacroKey> Key = keyFor(MacroNameTok);
        if (!Key) {
            return;
        }

        SourceLocation Location =
            SM.getSpellingLoc(MacroNameTok.getLocation());
        const bool DefinedHere = LocallyDefinedMacros.erase(*Key) != 0;
        if (!DefinedHere && !SM.isInSystemHeader(Location)) {
            Check.diag(Location,
                       "macro '%0' should only be undefined after a definition "
                       "in the same file")
                << MacroNameTok.getIdentifierInfo()->getName();
        }
    }
};

} // namespace

void SdcUndefSameFileCheck::registerPPCallbacks(
    const SourceManager& SM, Preprocessor* PP, Preprocessor*) {
    PP->addPPCallbacks(
        std::make_unique<UndefSameFileCallbacks>(*this, SM));
}

} // namespace sdc
} // namespace tidy
} // namespace clang
