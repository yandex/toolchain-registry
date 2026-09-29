// driver.cpp — System-Wide CTU Checker, fact extractor (schema v5, T0.1).
//
// A clean rewrite of the clang LibTooling extractor. Invoked as a compiler
// replacement for one TU:
//
//   ctu-extractor --ctu-source-root=<root> -c foo.cpp -o foo.blob <flags...>
//
// A frontend error fails the build (non-zero exit) by default, since a
// partial TU invalidates the whole-program result. --ctu-keep-going opts
// into exit 0 with a partial blob (still BlobHeader.incomplete=true).
//
// It runs the clang frontend, emits schema-v5 facts (see schema/factspb/facts.proto),
// and writes ONE deterministic, zstd-compressed `Blob` to the -o path. Facts
// are collected in memory, then canonically sorted + deduped + serialized with
// protobuf's deterministic option, so output is byte-identical run-to-run and
// independent of AST traversal order.
//
// Scope (T0.1, "minimal extractor proving the ACs"):
//   - decl/type facts (func/var/class/global_delete) carrying stable entity IDs
//   - a first cut of Access + ExprNode facts for volatile accesses (spine smoke)
// Deferred: per-function streaming + fork/pipe crash isolation (T0.5); the
// Steensgaard/footprint/call-graph facts (W3 / Stage 2), which are defined in
// the schema but not populated here.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "clang/Basic/Version.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <zstd.h>

#include "factspb/facts.pb.h"
#include "fact_visitor.h"
#include "source_utils.h"

static constexpr uint32_t kSchemaVersion = 5;
static constexpr const char *kToolVersion = "swc-extractor/0.1";

class FactConsumer : public ASTConsumer {
public:
  explicit FactConsumer(ASTContext &Ctx) : Visitor(Ctx) {}
  void HandleTranslationUnit(ASTContext &Ctx) override {
    Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  }

private:
  FactVisitor Visitor;
};

class FactAction : public ASTFrontendAction {
public:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef) override {
    return std::make_unique<FactConsumer>(CI.getASTContext());
  }
};

// ---------------------------------------------------------------------------
// Serialization / output
// ---------------------------------------------------------------------------

static std::string serializeDet(const google::protobuf::Message &msg) {
  std::string out;
  {
    google::protobuf::io::StringOutputStream sos(&out);
    google::protobuf::io::CodedOutputStream cos(&sos);
    cos.SetSerializationDeterministic(true);
    msg.SerializeToCodedStream(&cos);
  }
  return out;
}

// Returns false on failure; caller must hard-fail, not fall back to writing
// the uncompressed body (that used to fail-closed only by accident).
static bool zstdCompress(StringRef data, std::string *out) {
  size_t bound = ZSTD_compressBound(data.size());
  out->resize(bound);
  size_t n = ZSTD_compress(out->data(), bound, data.data(), data.size(),
                           /*level=*/9);
  if (ZSTD_isError(n)) return false;
  out->resize(n);
  return true;
}

static bool isSourceFile(StringRef a) {
  static const char *kExts[] = {".c", ".cc", ".cpp", ".cxx", ".c++", ".C"};
  for (const char *e : kExts)
    if (a.ends_with(e)) return true;
  return false;
}

static void touchEmpty(StringRef path) {
  if (path.empty()) return;
  llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path));
  std::error_code EC;
  llvm::raw_fd_ostream out(path, EC, llvm::sys::fs::OF_None);
}

int main(int argc, char **argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  std::vector<std::string> args;
  std::string outputPath, sourceFile;
  bool keepGoing = false;
  for (int i = 1; i < argc; ++i) {
    StringRef a = argv[i];
    if (a.starts_with("--ctu-source-root=")) {
      g_sourceRoot = a.drop_front(strlen("--ctu-source-root=")).str();
      continue;  // our flag; do not forward to clang
    }
    if (a == "--ctu-keep-going") {
      keepGoing = true;  // opt into exit 0 + best-effort partial blob
      continue;  // our flag; do not forward to clang
    }
    args.push_back(argv[i]);
    if (a == "-o" && i + 1 < argc) {
      outputPath = argv[i + 1];
    } else if (a.starts_with("-o") && a.size() > 2) {
      outputPath = a.drop_front(2).str();
    } else if (isSourceFile(a)) {
      sourceFile = std::string(a);
    }
  }

  if (sourceFile.empty()) {
    if (!outputPath.empty()) touchEmpty(outputPath);
    return 0;  // not a single-file compile (probe / link step)
  }

  std::vector<std::string> cmd;
  // Preserve the installed executable path so Clang can locate its packaged
  // lib/clang/<version>/include resource directory relative to argv[0].
  cmd.push_back(argv[0]);
  cmd.insert(cmd.end(), args.begin(), args.end());

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS =
      llvm::vfs::getRealFileSystem();
  auto Files = llvm::makeIntrusiveRefCnt<FileManager>(FileSystemOptions(), FS);
  tooling::ToolInvocation Invocation(cmd, std::make_unique<FactAction>(),
                                     Files.get());
  bool ok = Invocation.run();

  // Build the Blob: header + canonically sorted + deduped facts.
  facts_pb::Blob blob;
  facts_pb::BlobHeader *h = blob.mutable_header();
  h->set_schema_version(kSchemaVersion);
  h->set_tool_version(kToolVersion);
  h->set_toolchain_version(clang::getClangFullVersion());
  h->set_zstd_version(ZSTD_versionString());
  h->set_source_root(g_sourceRoot);
  // Partial facts may still be sitting in g_facts on a frontend error;
  // stamp it so the analyzer can't mistake this for a clean extraction.
  h->set_incomplete(!ok);
  h->set_has_complete_odr_definition_coverage(true);

  std::vector<std::string> serialized;
  serialized.reserve(g_facts.size());
  for (auto &f : g_facts) serialized.push_back(serializeDet(f));
  std::sort(serialized.begin(), serialized.end());
  serialized.erase(std::unique(serialized.begin(), serialized.end()),
                   serialized.end());
  for (auto &s : serialized) {
    facts_pb::Fact *f = blob.add_facts();
    f->ParseFromString(s);
  }

  std::string body = serializeDet(blob);

  // An incomplete TU must not collapse to the 0-byte "nothing extracted" case.
  bool haveContent = !g_facts.empty() || !ok;
  std::string compressed;
  bool zstdOk = !haveContent || zstdCompress(body, &compressed);
  if (!zstdOk) {
    llvm::errs() << "ctu-extractor: ERROR: zstd compression failed\n";
    return 1;  // an explicit extractor error, not a silent uncompressed fallback
  }

  if (!outputPath.empty()) {
    llvm::sys::fs::create_directories(llvm::sys::path::parent_path(outputPath));
    std::error_code EC;
    llvm::raw_fd_ostream out(outputPath, EC, llvm::sys::fs::OF_None);
    if (!EC && haveContent) out << compressed;
    else if (!EC) { /* no facts, frontend ok: leave 0-byte file (unambiguous "nothing") */ }
  } else if (haveContent) {
    llvm::outs() << compressed;
  }

  // Fail the build by default on a frontend error; --ctu-keep-going opts
  // into exit 0 with a partial (still incomplete=true) blob.
  if (!ok && !keepGoing) return 1;
  return 0;
}
