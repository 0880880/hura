#include "error.h"

#include <iostream>

void parser_throw(ParserErrorKind error_kind, size_t position,
                  uint8_t offending_token, uint8_t expected) {
  switch (error_kind) {
  case PARSER_ERROR_KIND_EOF:
    std::cout << "Expected \"" << (int)expected << "\" got EOF instead.";
    return;
  case PARSER_ERROR_KIND_UNEXPECTED:
    std::cout << "Expected \"" << (int)expected << "\" got \""
              << (int)offending_token << "\" instead.";
    return;
  case PARSER_ERROR_KIND_OTHER:
    std::cout << "Internal parser error.";
    return;
  }
}