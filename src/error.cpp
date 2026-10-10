#include "error.hpp"

#include <iostream>

void parser_throw(ParserErrorKind error_kind, size_t position,
                  uint8_t offending_token, uint8_t *expected,
                  size_t expected_size) {
  switch (error_kind) {
  case ParserErrorKind::_EOF:
    std::cout << "Expected \"" << "..." << "\" got EOF instead.";
    break;
  case ParserErrorKind::UNEXPECTED:
    std::cout << "Expected \"" << "..." << "\" got \"" << (int)offending_token
              << "\" at " << position << " instead.";
    break;
  case ParserErrorKind::OTHER:
    std::cout << "Internal parser error.";
    break;
  }
}
