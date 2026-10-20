#include "error.hpp"
#include "lexer.h"
#include "parser.h"

#include <iostream>

void parser_throw(ParserErrorKind error_kind, size_t position,
                  uint8_t offending_token, uint8_t *expected,
                  size_t expected_size) {
  std::string exp = "";
  switch (error_kind) {
  case ParserErrorKind::_EOF:
    std::cout << "Expected \"" << "..." << "\" got EOF instead.";
    break;
  case ParserErrorKind::UNEXPECTED:
    for (size_t i = 0; i < expected_size; ++i) {
      if (i != 0) {
        exp += ", ";
      }
      exp += kind_name(expected[i]);
    }
    std::cout << "Expected \"" << exp << "\" got \""
              << token_name(offending_token) << "\" at " << position
              << " instead.";
    break;
  case ParserErrorKind::OTHER:
    std::cout << "Internal parser error.";
    break;
  }
  exit(1);
}
