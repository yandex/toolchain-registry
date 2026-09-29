// entity_id.h — the NORMATIVE, versioned entity-ID scheme (T0.1).
//
// entity_id = bytes[0..8) of SHA-256(canonical_key), interpreted BIG-ENDIAN
// into a uint64. This is qualification evidence: the byte layout below is a
// frozen requirement, and any change is a schema-version bump. The Go analyzer
// never recomputes this — it reads entity_id out of the fact and, for the
// collision self-check, compares the emitted canonical_key bytes verbatim.
//
// canonical_key layout (hand-defined; independent of protobuf wire bytes,
// which are not canonical across implementations):
//
//   u8      entity_kind_tag           (frozen; NEVER renumbered)
//   u8      tu_qualified ? 1 : 0
//   uvarint len(mangled) + mangled    (UTF-8, verbatim)
//   uvarint len(tu_identity) + tu_id  (empty iff !tu_qualified)
//
// External-linkage entities are NOT tu-qualified: the same entity in many TUs
// hashes identically (required for cross-TU joins, and for header-defined
// inline entities to dedupe to one id). The mangled name already encodes a
// function's signature, so signature is intentionally NOT part of the key —
// folding it in would blind Rule 6.2.2. Internal-linkage entities, lambdas and
// block-local statics ARE tu-qualified — by the source-root-relative path of
// the MAIN FILE OF THE COMPILING TU (never the defining file). Qualifying by
// the defining file would fuse header-defined internal-linkage entities
// across every TU that includes them — the exact entity-fusion FN this
// scheme exists to prevent.

#ifndef CTU_EXTRACTOR_ENTITY_ID_H
#define CTU_EXTRACTOR_ENTITY_ID_H

#include <cstdint>
#include <string>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SHA256.h"

namespace ctu {

// Frozen entity-kind tags. Aligned with the proto EntityKind enum values but
// defined here independently because these bytes are hashed and must never
// move even if the proto enum is reordered.
enum EntityKindTag : uint8_t {
  KIND_UNKNOWN = 0,
  KIND_FUNCTION = 1,
  KIND_VAR = 2,
  KIND_TYPE = 3,
  KIND_CALLSITE = 4,
};

// Append an unsigned LEB128 varint.
inline void appendUVarint(std::string &out, uint64_t v) {
  do {
    uint8_t b = v & 0x7f;
    v >>= 7;
    if (v) b |= 0x80;
    out.push_back(static_cast<char>(b));
  } while (v);
}

// Append a length-prefixed byte field.
inline void appendLenPrefixed(std::string &out, llvm::StringRef s) {
  appendUVarint(out, s.size());
  out.append(s.begin(), s.end());
}

// Build the canonical pre-image. `tuIdentity` is ignored (and treated empty)
// when !tuQualified.
inline std::string canonicalKey(EntityKindTag kind, llvm::StringRef mangled,
                                bool tuQualified, llvm::StringRef tuIdentity) {
  std::string key;
  key.push_back(static_cast<char>(kind));
  key.push_back(tuQualified ? 1 : 0);
  appendLenPrefixed(key, mangled);
  appendLenPrefixed(key, tuQualified ? tuIdentity : llvm::StringRef());
  return key;
}

// Low 8 bytes of SHA-256(data), big-endian, as a uint64.
inline uint64_t sha256Low64(llvm::StringRef data) {
  llvm::ArrayRef<uint8_t> ref(reinterpret_cast<const uint8_t *>(data.data()),
                              data.size());
  std::array<uint8_t, 32> digest = llvm::SHA256::hash(ref);
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | digest[i];
  return v;
}

// The full entity-id computation: id = sha256Low64(canonicalKey(...)).
inline uint64_t entityId(EntityKindTag kind, llvm::StringRef mangled,
                         bool tuQualified, llvm::StringRef tuIdentity) {
  return sha256Low64(canonicalKey(kind, mangled, tuQualified, tuIdentity));
}

}  // namespace ctu

#endif  // CTU_EXTRACTOR_ENTITY_ID_H
