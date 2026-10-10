#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "base.h"
#include "lexer.h"
#include "parser.h"

int main() {
  std::ifstream file("test.hur", std::ios::binary);
  if (!file) {
    std::cerr << "error: could not open test.hur\n";
    return 1;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();

  const char8_t *data = reinterpret_cast<const char8_t *>(text.data());
  const size_t size = text.size();

  std::vector<uint8_t> tokens_kind;
  std::vector<const char8_t *> tokens_ptr;
  std::vector<size_t> tokens_len;

  size_t pos = 0;
  while (true) {
    size_t token_start = 0, token_end = 0;

    uint8_t kind = next_token(data + pos, size - pos, &token_start, &token_end);

    const size_t abs_start = pos + token_start;

    if (kind == TOKEN_KIND_NULL) {
      std::cerr << "error: unexpected character '" << text[abs_start]
                << "' at offset " << abs_start << "\n";
      pos = abs_start + 1;
      continue;
    }

    const size_t abs_end = pos + token_end;

    tokens_kind.push_back(kind);
    tokens_ptr.push_back(data + abs_start);
    tokens_len.push_back(abs_end - abs_start);

    std::cout << "token kind " << (int)kind << " [" << abs_start << ", "
              << abs_end << "): '"
              << text.substr(abs_start, abs_end - abs_start) << "'\n";

    pos = abs_end;

    if (kind == TOKEN_KIND_EOF) {
      break;
    }
  }

  Nodes nodes;

  parse(&nodes, tokens_kind.data(), tokens_kind.size());

  std::cout << "Len: " << nodes.len << std::endl;

  return 0;
}
