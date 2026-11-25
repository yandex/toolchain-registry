#pragma once

#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Core/Diagnostic.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace clang::tidy::sdc {

struct DeviationEntry {
    std::string SourceFile;
    std::string ID;
    int64_t Count = 0;
};

struct DeviationRegistry {
    std::string Root;
    std::vector<DeviationEntry> Deviations;
    std::vector<std::string> ExcludeDirs;
};
} // namespace clang::tidy::sdc

LLVM_YAML_IS_SEQUENCE_VECTOR(clang::tidy::sdc::DeviationEntry)

namespace llvm::yaml {
template <> struct MappingTraits<clang::tidy::sdc::DeviationEntry> {
    static void mapping(IO &IO, clang::tidy::sdc::DeviationEntry &Entry) {
        IO.mapRequired("source_file", Entry.SourceFile);
        IO.mapRequired("deviation_id", Entry.ID);
        IO.mapRequired("count", Entry.Count);
    }
};

template <> struct MappingTraits<clang::tidy::sdc::DeviationRegistry> {
    static void mapping(IO &IO, clang::tidy::sdc::DeviationRegistry &Registry) {
        IO.mapRequired("root", Registry.Root);
        IO.mapRequired("deviations", Registry.Deviations);
        IO.mapOptional("exclude_dirs", Registry.ExcludeDirs);
    }
};
} // namespace llvm::yaml

namespace clang::tidy::sdc {
// Used by the frontend's NOLINT handler, not by an optional tidy check. Counts
// are lexical source occurrences, never a budget consumed by diagnostics/TUs.
class ApprovedDeviations {
public:
    void configure(llvm::StringRef File) {
        Files.clear();
        const std::string Path = File.empty() ? "" : absolutePath(File, "");
        if (RegistryFile == Path) return;
        RegistryFile = Path;
        Loaded = false;
        RegistryError.clear();
        Limits.clear();
        ExcludeDirs.clear();
    }

    bool allows(const SourceManager &SM, FileID File, llvm::StringRef Buffer,
                unsigned Offset,
                llvm::SmallVectorImpl<tooling::Diagnostic> &Errors) {
        if (Manager != &SM) {
            Manager = &SM;
            Files.clear();
        }
        load();
        auto &State = Files[File.getHashValue()];
        if (!State.Scanned) {
            State.Scanned = true;
            if (const auto Entry = SM.getFileEntryRefForID(File))
                State.Path = absolutePath(Entry->getName(), "");
            State.Excluded = llvm::any_of(ExcludeDirs, [&](const std::string &Dir) {
                return isBelow(State.Path, Dir);
            });
            if (!State.Excluded) scan(SM, File, Buffer, State);
        }
        if (State.Excluded) return true;
        const auto Found = State.Directives.find(Offset);
        std::string Reason;
        if (!RegistryError.empty()) {
            Reason = RegistryError;
        } else if (Found == State.Directives.end() || Found->second.empty()) {
            Reason = "SDC NOLINT requires a trailing SDC-DEVIATION(id) annotation in the same comment";
        } else {
            const auto &ID = Found->second;
            const auto Limit = Limits.find({State.Path, ID});
            if (Limit == Limits.end())
                Reason = "deviation '" + ID + "' is not approved for this source file";
            else if (State.Counts[ID] > Limit->second)
                Reason = "deviation '" + ID + "' has " + std::to_string(State.Counts[ID]) +
                         " directives/blocks in this source file; approved maximum is " +
                         std::to_string(Limit->second);
            else
                return true;
        }
        if (State.Reported.insert(Offset).second) {
            tooling::Diagnostic Error;
            Error.DiagLevel = tooling::Diagnostic::Error;
            Error.DiagnosticName = "sdc-unapproved-deviation";
            Error.Message = tooling::DiagnosticMessage(
                Reason, SM, SM.getComposedLoc(File, Offset));
            Errors.push_back(std::move(Error));
        }
        return false;
    }

private:
    using Key = std::pair<std::string, std::string>;
    struct FileState {
        bool Scanned = false;
        bool Excluded = false;
        std::string Path;
        std::map<unsigned, std::string> Directives;
        std::map<std::string, uint64_t> Counts;
        std::set<unsigned> Reported;
    };

    static std::string absolutePath(llvm::StringRef Path, llvm::StringRef Base) {
        llvm::SmallString<256> Result(Path);
        if (llvm::sys::path::is_relative(Result)) {
            Result = Base;
            llvm::sys::path::append(Result, Path);
        }
        llvm::sys::fs::make_absolute(Result);
        llvm::sys::path::remove_dots(Result, true);
        llvm::SmallString<256> Real;
        if (!llvm::sys::fs::real_path(Result, Real)) Result = Real;
        return std::string(Result);
    }

    static bool isBelow(llvm::StringRef Path, llvm::StringRef Dir) {
        return Path.consume_front(Dir) && !Path.empty() &&
               (llvm::sys::path::is_separator(Dir.back()) || llvm::sys::path::is_separator(Path.front()));
    }

    static bool validID(llvm::StringRef ID) {
        return !ID.empty() && llvm::all_of(ID, [](char C) {
            return llvm::isAlnum(C) || C == '-' || C == '_' || C == '.';
        });
    }

    void invalid(llvm::StringRef Reason) {
        RegistryError = "invalid SDC deviations file '" + RegistryFile + "': " + Reason.str();
        Limits.clear();
        ExcludeDirs.clear();
    }

    void load() {
        if (Loaded) return;
        Loaded = true;
        if (RegistryFile.empty()) return; // No registry means no approvals.
        const auto Input = llvm::MemoryBuffer::getFile(RegistryFile);
        if (!Input) {
            invalid(Input.getError().message());
            return;
        }
        DeviationRegistry Registry;
        llvm::yaml::Input YAML((*Input)->getBuffer());
        YAML >> Registry;
        if (YAML.error() || Registry.Root.empty()) {
            invalid("expected YAML with root and deviations (source_file, deviation_id, count)");
            return;
        }
        if (YAML.nextDocument()) {
            invalid("expected a single YAML document");
            return;
        }
        const std::string RegistryPath = absolutePath(RegistryFile, "");
        const std::string Root = absolutePath(Registry.Root,
                                              llvm::sys::path::parent_path(RegistryPath));
        for (const std::string &Dir : Registry.ExcludeDirs) {
            const auto Path = absolutePath(Dir, Root);
            if (Dir.empty() || llvm::sys::path::is_absolute(Dir) ||
                Dir.find_first_of("*?") != std::string::npos || !isBelow(Path, Root)) {
                invalid("expected exclude_dirs to contain literal relative directories beneath root");
                return;
            }
            ExcludeDirs.push_back(Path);
        }
        for (const auto &Entry : Registry.Deviations) {
            const llvm::StringRef Source = Entry.SourceFile;
            const llvm::StringRef ID = Entry.ID;
            const auto Count = Entry.Count;
            if (Source.empty() || llvm::sys::path::is_absolute(Source) ||
                Source.contains('*') || Source.contains('?') || !validID(ID) || Count < 0) {
                invalid("expected a relative literal source path, an ID, and nonnegative count");
                return;
            }
            if (!Limits.emplace(Key{absolutePath(Source, Root), ID.str()}, Count).second) {
                invalid("duplicate source_file/deviation_id entry");
                return;
            }
        }
    }

    // Count only real comments. A string literal containing an approval cannot
    // authorize a NOLINT, even though upstream searches raw source text.
    static void scan(const SourceManager &SM, FileID File, llvm::StringRef Buffer,
                     FileState &State) {
        LangOptions Lang;
        Lang.CPlusPlus = true;
        Lang.CPlusPlus11 = true;
        Lang.LineComment = true;
        Lexer Lex(SM.getLocForStartOfFile(File), Lang, Buffer.begin(), Buffer.begin(), Buffer.end());
        Lex.SetCommentRetentionState(true);
        Token Tok;
        bool AtEnd = false;
        while (!AtEnd) {
            // LexFromRawLexer can reach EOF while still returning the final
            // comment token (for a source file without a trailing newline).
            AtEnd = Lex.LexFromRawLexer(Tok);
            if (!Tok.is(tok::comment)) continue;
            unsigned Base = SM.getFileOffset(Tok.getLocation());
            llvm::StringRef Comment = Buffer.substr(Base, Tok.getLength());
            while (!Comment.empty()) {
                const auto Split = Comment.split('\n');
                const auto Line = Split.first;
                size_t Pos = 0;
                while ((Pos = Line.find("NOLINT", Pos)) != llvm::StringRef::npos) {
                    const size_t Start = Pos;
                    Pos += 6;
                    while (Pos < Line.size() && llvm::isAlpha(Line[Pos])) ++Pos;
                    const auto Kind = Line.slice(Start, Pos);
                    if (Kind != "NOLINT" && Kind != "NOLINTNEXTLINE" && Kind != "NOLINTBEGIN") continue;
                    auto Tail = Line.substr(Pos);
                    if (Tail.starts_with("(")) {
                        const auto Close = Tail.find(')');
                        if (Close == llvm::StringRef::npos) continue;
                        Tail = Tail.drop_front(Close + 1);
                    }
                    constexpr llvm::StringLiteral Marker = "SDC-DEVIATION(";
                    const auto Tag = Tail.find(Marker);
                    std::string ID;
                    if (Tag != llvm::StringRef::npos && Tail.find("NOLINT") == llvm::StringRef::npos) {
                        const auto Rest = Tail.substr(Tag + Marker.size());
                        const auto End = Rest.find(')');
                        if (End != llvm::StringRef::npos && validID(Rest.take_front(End)) &&
                            Rest.drop_front(End + 1).find(Marker) == llvm::StringRef::npos)
                            ID = Rest.take_front(End).str();
                    }
                    State.Directives.emplace(Base + Start, ID);
                    if (!ID.empty()) ++State.Counts[ID];
                }
                Base += Line.size() + 1;
                Comment = Split.second;
            }
        }
    }

    std::string RegistryFile;
    bool Loaded = false;
    std::string RegistryError;
    std::map<Key, uint64_t> Limits;
    std::vector<std::string> ExcludeDirs;
    const SourceManager *Manager = nullptr;
    std::map<unsigned, FileState> Files;
};
} // namespace clang::tidy::sdc
