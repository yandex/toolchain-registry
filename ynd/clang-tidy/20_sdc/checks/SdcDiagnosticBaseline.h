#pragma once

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace clang::tidy::sdc {
    using BaselineChecks = std::vector<std::string>;
    struct DiagnosticBaselineData {
        std::string Root;
        std::map<std::string, BaselineChecks> Files;
    };
} // namespace clang::tidy::sdc

LLVM_YAML_IS_STRING_MAP(clang::tidy::sdc::BaselineChecks)

namespace llvm::yaml {
    template <>
    struct MappingTraits<clang::tidy::sdc::DiagnosticBaselineData> {
        static void mapping(IO& IO, clang::tidy::sdc::DiagnosticBaselineData& Data) {
            IO.mapRequired("root", Data.Root);
            IO.mapRequired("files", Data.Files);
        }
    };
} // namespace llvm::yaml

namespace clang::tidy::sdc {

    // A frozen file/check set, independent of NOLINT approvals. Never consumes counts.
    class DiagnosticBaseline {
    public:
        void configure(llvm::StringRef File) {
            const auto Path = File.empty() ? std::string() : absolutePath(File, "");
            Reported = false;
            if (Path == RegistryFile) {
                return;
            }
            RegistryFile = Path;
            Error.clear();
            Entries.clear();
            if (File.empty()) {
                return;
            }
            auto Buffer = llvm::MemoryBuffer::getFile(Path);
            if (!Buffer) {
                invalid(Buffer.getError().message());
                return;
            }
            DiagnosticBaselineData Data;
            llvm::yaml::Input YAML((*Buffer)->getBuffer());
            YAML >> Data;
            if (YAML.error() || Data.Root.empty()) {
                invalid("expected root and files mapping");
                return;
            }
            const bool More = YAML.nextDocument();
            if (More || YAML.error()) {
                invalid("expected one YAML document");
                return;
            }
            const auto Root = absolutePath(Data.Root, llvm::sys::path::parent_path(Path));
            for (const auto& [Source, Checks] : Data.Files) {
                if (Source.empty() || llvm::sys::path::is_absolute(Source) ||
                    Source.find_first_of("*?") != std::string::npos || Checks.empty()) {
                    invalid("expected literal relative file paths and nonempty check lists");
                    return;
                }
                const auto FullPath = absolutePath(Source, Root);
                for (const auto& Check : Checks) {
                    if (!eligible(Check) || !Entries.emplace(FullPath, Check).second) {
                        invalid("expected unique file/check pairs with literal sdc-* check names");
                        return;
                    }
                }
            }
        }

        static bool eligible(llvm::StringRef Check) {
            return Check.starts_with("sdc-") && Check.size() > 4 &&
                   Check != "sdc-unapproved-deviation" && Check != "sdc-invalid-baseline" &&
                   llvm::all_of(Check, [](char C) {
                       return llvm::isAlnum(C) || C == '-';
                   });
        }

        bool matches(llvm::StringRef File, llvm::StringRef Check) const {
            return Error.empty() && eligible(Check) &&
                   Entries.count({absolutePath(File, ""), Check.str()});
        }

        std::string takeError() {
            if (Reported) {
                return {};
            }
            Reported = true;
            return Error;
        }

    private:
        static std::string absolutePath(llvm::StringRef Path, llvm::StringRef Base) {
            llvm::SmallString<256> Result(Path);
            if (llvm::sys::path::is_relative(Result)) {
                Result = Base;
                llvm::sys::path::append(Result, Path);
            }
            llvm::sys::fs::make_absolute(Result);
            llvm::sys::path::remove_dots(Result, true);
            llvm::SmallString<256> Real;
            if (!llvm::sys::fs::real_path(Result, Real)) {
                Result = Real;
            }
            return std::string(Result);
        }

        void invalid(llvm::StringRef Reason) {
            Error = "invalid SDC baseline '" + RegistryFile + "': " + Reason.str();
            Entries.clear();
        }

        std::string RegistryFile;
        std::string Error;
        bool Reported = false;
        std::set<std::pair<std::string, std::string>> Entries;
    };
} // namespace clang::tidy::sdc
