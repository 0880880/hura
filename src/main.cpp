#include <filesystem>

#include "base.h"
#include "compiler.hpp"
#include "lexer.h"
#include "parser.h"

int main() {
  std::filesystem::path dir = ".";
  std::filesystem::path source = dir / "test.hur";
  Compiler::schedule(source);

  Compiler::start();

  return 0;
}
