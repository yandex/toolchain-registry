#include "bridge_header.h"

#include "empty_catch_all_check.h"
#include "pragma_once_check.h"

namespace clang::tidy::eats {

class EatsModule : public ClangTidyModule {
public:
    void addCheckFactories(ClangTidyCheckFactories& checkFactories) override {
        checkFactories.registerCheck<EmptyCatchAllCheck>("eats-empty-catch-all");
        checkFactories.registerCheck<PragmaOnceCheck>("eats-pragma-once");
    }
};

static ClangTidyModuleRegistry::Add<EatsModule> X("eats-module", "Adds Yandex Eats codestyle checks.");

}  // namespace clang::tidy::eats

namespace clang::tidy {
// This anchor is used to force the linker to link in the generated object file
// and thus register the EatsModule.
volatile int EatsModuleAnchorSource = 0;
}  // namespace clang::tidy
