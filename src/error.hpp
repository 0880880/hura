#include <stdint.h>

enum class ParserErrorKind : uint8_t { UNEXPECTED = 0, _EOF = 1, OTHER = 2 };

void parser_throw(ParserErrorKind error_kind, size_t position,
                  uint8_t offending_token, uint8_t *expected,
                  size_t expected_size);