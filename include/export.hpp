#pragma once

#include "type.hpp"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

struct FunctionExport {
  std::string name;
  TypeRef return_type;
  std::string return_name;
  std::vector<TypeRef> param_types;
  std::vector<std::string> param_names;
  bool is_vararg;
};

struct StructExport {
  std::string name;
  std::vector<TypeRef> var_types;
  std::vector<std::string> var_names;
  std::vector<FunctionExport> functions;
  bool visible;
};

struct EnumExport {
  std::string name;
  std::vector<std::string> names;
};

struct Export {
  enum class Kind : uint8_t {
    Function,
    Struct,
    Enum,
  };

  Kind kind;
  std::variant<FunctionExport, StructExport, EnumExport> data;

  Export() : kind(Kind::Function), data(FunctionExport{}) {}
};
