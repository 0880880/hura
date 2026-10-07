#include <stdint.h>

enum ParserErrorKind {
  PARSER_ERROR_KIND_UNEXPECTED = 0,
  PARSER_ERROR_KIND_EOF = 1,
  PARSER_ERROR_KIND_OTHER = 2
};

void parser_throw(ParserErrorKind error_kind, size_t position,
                  uint8_t offending_token, uint8_t expected);