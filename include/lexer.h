#pragma once

#include <stdint.h>
#include "base.h"
#define TOKEN_KIND_0 2
#define TOKEN_KIND_1 3
extern "C" uint8_t next_token(const char8_t *text, size_t len, size_t *token_start, size_t *token_end);
