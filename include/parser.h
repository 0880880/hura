#pragma once

#include <stdint.h>
#include "base.h"
#define NONTERMINAL_ID_A 0
#define NONTERMINAL_ID_A_p 4
#define NONTERMINAL_ID_B 1
#define NONTERMINAL_ID_C 2
#define NONTERMINAL_ID_D 3

struct Node {
  uint8_t kind;
  uint8_t *start;
  uint8_t *end;
  uint8_t children_count;
  uint32_t subtree_size;
};

struct Nodes {
  Node *nodes;
  size_t len;
  size_t cap;
};
extern "C" [[noreturn]] void parse(Nodes *out_nodes, uint8_t* tokens, size_t len);
