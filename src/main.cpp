#include <filesystem>

#include "compiler.hpp"

int main() {
  std::filesystem::path dir = ".";
  std::filesystem::path source = dir / "test.hur";
  Compiler::schedule(source);

  Compiler::start();

  return 0;
}
