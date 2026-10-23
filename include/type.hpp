#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_set>

struct Type {
  enum class Kind : uint8_t {
    IntLiteral,
    FloatLiteral,
    Pointer,
    Reference,
    MetaType,
    UserType,
    Array,
    Func,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    Void,
  };

  Kind kind;
  uint64_t payload = 0;
  const Type *inner = nullptr;
  std::span<const Type *const> params{};

  bool is(Kind k) const { return kind == k; }
  bool is_pointer() const { return kind == Kind::Pointer; }
  bool is_func() const { return kind == Kind::Func; }
};

using TypeRef = const Type *;

namespace detail {
constexpr uint64_t P1 = 0x9E3779B185EBCA87ULL;
constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4FULL;
constexpr uint64_t P3 = 0x165667B19E3779F9ULL;
constexpr uint64_t P4 = 0x85EBCA77C2B2AE63ULL;
constexpr uint64_t P5 = 0x27D4EB2F165667C5ULL;

inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

inline uint64_t round64(uint64_t acc, uint64_t input) {
  acc += input * P2;
  acc = rotl(acc, 31);
  return acc * P1;
}
inline uint64_t merge(uint64_t acc, uint64_t val) {
  acc ^= round64(0, val);
  return acc * P1 + P4;
}
inline uint64_t avalanche(uint64_t h) {
  h ^= h >> 33;
  h *= P2;
  h ^= h >> 29;
  h *= P3;
  h ^= h >> 32;
  return h;
}
inline uint64_t hash_type(const Type &t) {
  uint64_t h = P5;
  h = merge(h, static_cast<uint64_t>(t.kind));
  h = merge(h, t.payload);
  h = merge(h, reinterpret_cast<uintptr_t>(t.inner));
  for (const Type *p : t.params)
    h = merge(h, reinterpret_cast<uintptr_t>(p));
  h += 8 * (2 + t.params.size());
  return avalanche(h);
}
inline bool type_eq(const Type &a, const Type &b) {
  return a.kind == b.kind && a.payload == b.payload && a.inner == b.inner &&
         std::equal(a.params.begin(), a.params.end(), b.params.begin(),
                    b.params.end());
}

struct PtrHash {
  size_t operator()(const Type *t) const { return hash_type(*t); }
};
struct PtrEq {
  bool operator()(const Type *a, const Type *b) const {
    return type_eq(*a, *b);
  }
};

} // namespace detail

class TypeInterner {
  static constexpr size_t kShards = 64; // power of 2

  struct Node {
    Type type;
    std::unique_ptr<const Type *[]> params_storage;
  };

  struct Shard {
    std::shared_mutex mutex;
    std::unordered_set<const Type *, detail::PtrHash, detail::PtrEq> set;
    std::deque<Node> nodes;
  };

  std::array<Shard, kShards> shards;

  TypeRef intern(const Type &candidate);

public:
  TypeRef make_primitive(Type::Kind k);

  TypeRef make_pointer(TypeRef inner);

  TypeRef make_reference(TypeRef inner);

  TypeRef make_array(TypeRef elem, uint64_t len);

  TypeRef make_func(TypeRef ret, std::span<const Type *const> params);
};
