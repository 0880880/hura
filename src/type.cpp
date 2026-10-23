#include "type.hpp"

TypeRef TypeInterner::intern(const Type &candidate) {
  const uint64_t h = detail::hash_type(candidate);
  Shard &sh = shards[h >> 58];

  {
    std::shared_lock lock(sh.mutex);
    if (auto it = sh.set.find(&candidate); it != sh.set.end())
      return *it;
  }

  std::unique_lock lock(sh.mutex);
  if (auto it = sh.set.find(&candidate); it != sh.set.end()) {
    return *it;
  }

  Node &n = sh.nodes.emplace_back();
  n.type = candidate;

  if (!candidate.params.empty()) {
    n.params_storage =
        std::make_unique<const Type *[]>(candidate.params.size());
    std::copy(candidate.params.begin(), candidate.params.end(),
              n.params_storage.get());
    n.type.params = {n.params_storage.get(), candidate.params.size()};
  }

  sh.set.insert(&n.type);
  return &n.type;
}

TypeRef TypeInterner::make_primitive(Type::Kind k) {
  return intern({.kind = k});
}

TypeRef TypeInterner::make_pointer(TypeRef inner) {
  return intern({.kind = Type::Kind::Pointer, .inner = inner});
}

TypeRef TypeInterner::make_reference(TypeRef inner) {
  return intern({.kind = Type::Kind::Reference, .inner = inner});
}

TypeRef TypeInterner::make_array(TypeRef elem, uint64_t len) {
  return intern({.kind = Type::Kind::Array, .payload = len, .inner = elem});
}

TypeRef TypeInterner::make_func(TypeRef ret,
                                std::span<const Type *const> params) {
  return intern({.kind = Type::Kind::Func, .inner = ret, .params = params});
}
