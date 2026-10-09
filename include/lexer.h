#include <stdint.h>
#include "base.h"
#define TOKEN_KIND_0 2
#define TOKEN_KIND_1 3
int8_t next_token(const uint8_t *text, size_t len, uint8_t *token_start, uint8_t *token_end);
