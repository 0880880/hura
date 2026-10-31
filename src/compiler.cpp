#include "compiler.hpp"

#include "base.h"
#include "lexer.h"
#include "parser.h"

#include <assert.h>
#include <cstddef>
#include <iostream>
#include <latch>
#include <string_view>
#include <thread>
#include <vector>

std::string_view Module::get_line(size_t line) const {
  if (starts.empty() || line >= starts.size()) {
    throw std::out_of_range("Line index out of bounds");
  }

  size_t start = static_cast<size_t>(starts[line]);

  size_t end = (line + 1 < starts.size())
                   ? static_cast<size_t>(starts[line + 1])
                   : view.size();

  if (end > start && view[end - 1] == '\n') {
    end -= 1;
  }

  return view.substr(start, end - start);
}

static thread_local size_t current_file;
static std::vector<Module> modules;

Module &Compiler::get_module() { return modules[current_file]; }

void Compiler::schedule(std::filesystem::path &path) {
  modules.push_back(Module(path));
}

inline void lex(std::vector<uint8_t> &tokens_kind,
                std::vector<const char *> &tokens_ptr,
                std::vector<size_t> &tokens_len) {

  Module &mod = Compiler::get_module();

  size_t pos = 0;
  while (true) {
    size_t token_start = 0, token_end = 0;

    uint8_t kind = next_token(mod.view.data() + pos, mod.view.size() - pos,
                              &token_start, &token_end);

    const size_t abs_start = pos + token_start;

    if (kind == TOKEN_KIND_NULL) {
      std::cerr << "error: unexpected character '" << mod.view[abs_start]
                << "' at offset " << abs_start << "\n";
      pos = abs_start + 1;
      continue;
    }

    const size_t abs_end = pos + token_end;

    tokens_kind.push_back(kind);
    tokens_ptr.push_back(mod.view.data() + abs_start);
    tokens_len.push_back(abs_end - abs_start);

    std::cout << "token kind " << token_name(kind) << " [" << abs_start << ", "
              << abs_end << "): '"
              << mod.view.substr(abs_start, abs_end - abs_start) << "'\n";

    pos = abs_end;

    if (kind == TOKEN_KIND_EOF) {
      break;
    }
  }
}

void dump(const Nodes &n, std::vector<uint8_t> &tokens_kind,
          std::vector<const char *> &tokens_ptr,
          std::vector<size_t> &tokens_len, size_t i, int depth = 0) {
  const Node &node = n.nodes[i];
  printf("%*s", depth * 2, "");
  std::cout << kind_name(node.kind);
  if (node.kind < 128) {
    std::cout << ": \"";
    std::cout << std::string_view{tokens_ptr[node.start_token],
                                  tokens_len[node.start_token]};
    std::cout << "\"";
  }
  std::cout << "\n";

  std::vector<size_t> kids;
  size_t c = i - 1;
  for (int k = 0; k < node.children_count; k++) {
    kids.push_back(c);
    c -= n.nodes[c].subtree_size;
  }
  std::reverse(kids.begin(), kids.end());
  for (size_t kid : kids) {
    dump(n, tokens_kind, tokens_ptr, tokens_len, kid, depth + 1);
  }
}

constexpr inline std::string_view substr(Module &mod, Node &node) {
  return std::string_view{mod.tokens_ptr[node.start_token],
                          mod.tokens_len[node.start_token]};
}

void process(std::latch &sync) {

  Module &mod = Compiler::get_module();

  lex(mod.tokens_kind, mod.tokens_ptr, mod.tokens_len);

  Nodes nodes;

  parse(&nodes, mod.tokens_kind.data(), mod.tokens_kind.size());

  dump(nodes, mod.tokens_kind, mod.tokens_ptr, mod.tokens_len, nodes.len - 1);

  for (size_t i = nodes.len - 1; i > 0;) {
    if (nodes.nodes[i].kind == NONTERMINAL_ID_PROGRAM ||
        nodes.nodes[i].kind == NONTERMINAL_ID_TL_STATEMENT) {
      i--;
      continue;
    } else if (nodes.nodes[i].kind == NONTERMINAL_ID_IMPORT_S) {
      Node &lib_node = nodes.nodes[i - 4];
      Node &mod_node = nodes.nodes[i - 2];
      mod.imports.emplace_back(
          substr(mod, lib_node),
          substr(mod, mod_node));
    } else if (nodes.nodes[i].kind == NONTERMINAL_ID_STRUCT_S) {
      // find details
    } else if (nodes.nodes[i].kind == NONTERMINAL_ID_FUNCTION) {
      // find details
    } else {
      std::cout << "Skipping " << kind_name(nodes.nodes[i].kind) << std::endl;
    }
    i -= nodes.nodes[i].subtree_size;
  }

  sync.arrive_and_wait();
  // IR codegen
}

void Compiler::start() {
  constexpr int num_threads = 1;

  std::latch sync(num_threads);
  std::vector<std::jthread> pool;

  for (int i = 0; i < num_threads; ++i) {
    size_t from = 0, to = 1;

    pool.emplace_back([&sync, from, to] {
      for (size_t i = from; i < to; ++i) {
        current_file = i;
        process(sync);
      }
    });
  }
}
