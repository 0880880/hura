#include <cstdint>
#include <fstream>
#include <iostream>
#include <llvm/IR/Instructions.h>
#include <map>
#include <memory>
#include <set>
#include <stdint.h>
#include <string>
#include <system_error>
#include <vector>

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>

#include "base.h"

struct TokenPattern {
  std::string name;
  std::string pattern;

  TokenPattern(std::string name, std::string pattern)
      : name(name), pattern(pattern) {}
};

struct Node {
public:
  virtual ~Node() = default;
};

struct Literal : Node {
  bool escaped;
  char ch;

  Literal(char ch, bool escaped) {
    this->ch = ch;
    this->escaped = escaped;
  }
};

struct Plus : Node {
  std::shared_ptr<Node> inner;

  Plus(std::shared_ptr<Node> inner) { this->inner = std::move(inner); }
};

struct Star : Node {
  std::shared_ptr<Node> inner;

  Star(std::shared_ptr<Node> inner) { this->inner = std::move(inner); }
};

struct Question : Node {
  std::shared_ptr<Node> inner;

  Question(std::shared_ptr<Node> inner) { this->inner = std::move(inner); }
};

struct Set : Node {
  std::set<char> chars;
};

struct Concatenate : Node {
  std::shared_ptr<Node> a;
  std::shared_ptr<Node> b;

  Concatenate(std::shared_ptr<Node> a, std::shared_ptr<Node> b) {
    this->a = std::move(a);
    this->b = std::move(b);
  }
};

struct Alternate : Node {
  std::shared_ptr<Node> a;
  std::shared_ptr<Node> b;

  Alternate(std::shared_ptr<Node> a, std::shared_ptr<Node> b) {
    this->a = std::move(a);
    this->b = std::move(b);
  }
};

struct State {
  std::map<std::optional<char>, std::vector<std::shared_ptr<State>>>
      transitions;
  bool is_word_boundary;
  bool is_accepting;
  std::string token_name;

  void add_transition(std::optional<char> symbol,
                      std::shared_ptr<State> state) {
    transitions[symbol].push_back(state);
  }
};

struct Fragment {
  std::shared_ptr<State> start;
  std::vector<std::shared_ptr<State>> acceptings;
};

struct DFAState {
  int id;
  std::map<char, int> transitions;
  bool is_accepting;
  bool is_word_boundary;
  std::string token_name;
  llvm::BasicBlock *pre_start;
  llvm::BasicBlock *bb;
  llvm::BasicBlock *body_bb;
  llvm::PHINode *ws_i_phi;
  llvm::PHINode *i_phi;
  llvm::Value *i_next;
};

inline constexpr bool is_word_char(char c) {
  return std::isalnum((unsigned char)c) || c == '_';
}

void epsilon_closure(const std::set<std::shared_ptr<State>> &states,
                     std::set<std::shared_ptr<State>> &out) {
  std::vector<std::shared_ptr<State>> worklist(states.begin(), states.end());
  while (!worklist.empty()) {
    std::shared_ptr<State> s = worklist.back();
    worklist.pop_back();
    if (out.contains(s)) {
      continue;
    }
    out.insert(s);
    auto it = s->transitions.find(std::nullopt);
    if (it != s->transitions.end()) {
      for (const std::shared_ptr<State> &next : it->second) {
        worklist.push_back(next);
      }
    }
  }
}

std::set<std::shared_ptr<State>>
move(const std::set<std::shared_ptr<State>> &states, char ch) {
  std::set<std::shared_ptr<State>> out;
  for (const std::shared_ptr<State> &s : states) {
    auto it = s->transitions.find(ch);
    if (it != s->transitions.end()) {
      for (const std::shared_ptr<State> &next : it->second) {
        out.insert(next);
      }
    }
  }
  return out;
}

class Cursor {
public:
  Cursor() : str(""), idx(0) {}
  Cursor(std::string s) : str(std::move(s)), idx(0) {}

  inline constexpr char get() const {
    return idx < str.length() ? str[idx] : '\0';
  }

  inline constexpr bool eof() const { return idx >= str.length(); }

  inline constexpr void operator++() {
    if (idx < str.length())
      ++idx;
  }

  inline constexpr bool operator==(char c) const {
    return !eof() && str[idx] == c;
  }

  inline constexpr bool operator!=(char c) const {
    return eof() || str[idx] != c;
  }

private:
  std::string str;
  size_t idx;
};

struct StateDetails {
  std::string token_name;
  bool is_accepting;
  bool is_word_boundary;

  bool operator<(const StateDetails &other) const {
    if (token_name != other.token_name) {
      return token_name < other.token_name;
    }
    if (is_accepting != other.is_accepting) {
      return is_accepting < other.is_accepting;
    }
    return is_word_boundary < other.is_word_boundary;
  }
};

#define USER_TOKEN_OFFSET 2

class LexerBuilder {
public:
  std::vector<TokenPattern> tokens;
  std::vector<std::string> priority;

  void add(std::string name, std::string pat) {
    tokens.emplace_back(name, pat);
  }

  void build(const std::string out_header, std::string out_ir) {
    std::shared_ptr<State> root_start = std::make_unique<State>();

    priority.clear();

    for (size_t i = 0; i < tokens.size(); ++i) {
      std::vector<std::unique_ptr<Node>> nodes;
      TokenPattern tok = tokens[i];
      priority.push_back(tok.name);
      cursor = {tok.pattern};
      std::shared_ptr<Node> n = expr();
      Fragment frag = build_nfa(n);

      for (std::shared_ptr<State> s : frag.acceptings) {
        s->is_accepting = true;
        s->token_name = tok.name;
      }

      root_start->add_transition(std::nullopt, frag.start);
    }

    std::map<std::set<std::shared_ptr<State>>, int> dfa_ids;
    std::map<int, std::map<char, int>> dfa_transitions;
    std::map<int, std::string> dfa_accepting;
    dfa.clear();

    int next_id = 0;

    std::set<std::shared_ptr<State>> start_set;
    epsilon_closure({root_start}, start_set);
    dfa_ids[start_set] = next_id++;

    std::vector<std::set<std::shared_ptr<State>>> worklist = {start_set};

    std::set<char> alphabet;
    for (unsigned char c = 1; c < 127; ++c) {
      alphabet.insert(c);
    }

    while (!worklist.empty()) {
      std::set<std::shared_ptr<State>> T = worklist.back();
      worklist.pop_back();

      int T_id = dfa_ids[T];

      DFAState &current_dfa = dfa[T_id];
      current_dfa.id = T_id;

      for (const std::string &token_name : priority) {
        for (const std::shared_ptr<State> &s : T) {
          if (s->is_accepting && s->token_name == token_name) {
            current_dfa.is_accepting = true;
            current_dfa.token_name = token_name;
            goto done_accepting;
          }
        }
      }
    done_accepting:;

      for (char c : alphabet) {
        std::set<std::shared_ptr<State>> moved = move(T, c);
        if (moved.empty()) {
          continue;
        }

        std::set<std::shared_ptr<State>> U;
        epsilon_closure(moved, U);
        if (U.empty()) {
          continue;
        }

        if (!dfa_ids.contains(U)) {
          dfa_ids[U] = next_id++;
          worklist.push_back(U);
        }
        current_dfa.transitions[c] = dfa_ids.at(U);

        bool needs_boundary = false;
        for (const auto &s : U) {
          if (s->is_word_boundary) {
            needs_boundary = true;
            break;
          }
        }
        current_dfa.is_word_boundary = needs_boundary;
      }
    }

    int start_state_id = 0;

    bool optimize = true;
    if (optimize) { // Hopcroft
      std::map<StateDetails, std::set<int>> partition_labels = {};
      std::set<std::set<int>> partitions = {};
      for (auto const &[id, s] : dfa) {
        StateDetails det;
        det.is_accepting = s.is_accepting;
        det.is_word_boundary = s.is_word_boundary;
        det.token_name = s.token_name;
        partition_labels[det].insert(id);
      }
      std::set<int> smallest_non_empty = {};
      size_t smallest_size = 10000000000;
      for (auto const &[_, p] : partition_labels) {
        if (!p.empty()) {
          partitions.insert(p);
          if (p.size() < smallest_size) {
            smallest_size = p.size();
            smallest_non_empty = p;
          }
        }
      }

      std::vector<std::set<int>> worklist = {};
      if (!smallest_non_empty.empty()) {
        worklist.push_back(smallest_non_empty);
      }

      std::set<int> X;
      std::set<int> intersection;
      std::set<int> difference;
      std::vector<std::set<int>> to_insert;

      std::unordered_map<int, std::unordered_map<char, std::vector<int>>>
          rev_delta;
      for (auto const &[src, state] : dfa) {
        for (auto const &[symbol, target] : state.transitions) {
          rev_delta[target][symbol].push_back(src);
        }
      }
      while (!worklist.empty()) {
        std::set<int> A = worklist.back();
        worklist.pop_back();
        for (char c : alphabet) {
          X.clear();
          for (int sid : A) {
            for (int pred : rev_delta[sid][c]) {
              X.insert(pred);
            }
          }
          for (auto it = partitions.begin(); it != partitions.end();) {
            intersection.clear();
            difference.clear();
            for (int i : *it) {
              if (X.contains(i)) {
                intersection.insert(i);
              } else {
                difference.insert(i);
              }
            }

            std::set<int> Y = *it;
            if (!intersection.empty() && !difference.empty()) {
              it = partitions.erase(it);
              to_insert.push_back(intersection);
              to_insert.push_back(difference);

              auto found = std::find(worklist.begin(), worklist.end(), Y);
              if (found != worklist.end()) {
                worklist.erase(found);
                worklist.push_back(intersection);
                worklist.push_back(difference);
              } else {
                if (intersection.size() < difference.size()) {
                  worklist.push_back(intersection);
                } else {
                  worklist.push_back(difference);
                }
              }
            } else {
              ++it;
            }
          }
          for (std::set<int> Y : to_insert) {
            partitions.insert(Y);
          }
          to_insert.clear();
        }
      }

      std::map<int, DFAState> pruned_dfa;

      std::unordered_map<int, int> state_to_new_id;
      for (const std::set<int> &partition : partitions) {
        int rep_id = *partition.begin();
        for (int old_id : partition) {
          state_to_new_id[old_id] = rep_id;
        }
      }

      start_state_id = state_to_new_id.at(0);

      for (const std::set<int> &partition : partitions) {
        int rep_id = *partition.begin();
        const DFAState &rep_state = dfa.at(rep_id);

        DFAState &new_state = pruned_dfa[rep_id];

        new_state.is_accepting = rep_state.is_accepting;
        new_state.is_word_boundary = rep_state.is_word_boundary;
        new_state.token_name = rep_state.token_name;

        for (const auto &[symbol, target_id] : rep_state.transitions) {
          if (state_to_new_id.contains(target_id)) {
            new_state.transitions[symbol] = state_to_new_id.at(target_id);
          }
        }
      }

      dfa.clear();
      for (auto const &[i, s] : pruned_dfa) {
        dfa[i] = s;
      }
    }

    context = std::make_unique<llvm::LLVMContext>();

    std::unique_ptr<llvm::Module> module =
        std::make_unique<llvm::Module>("lexer", *context);

    llvm::IRBuilder<> builder(*context);

    i1_type = llvm::Type::getInt1Ty(*context);
    i8_type = llvm::Type::getInt8Ty(*context);
    i64_type = llvm::Type::getInt64Ty(*context);
    llvm::PointerType *ptr_type = llvm::PointerType::get(*context, 0);

    { // word_lookup_table

      std::vector<llvm::Constant *> chars;
      for (unsigned char c = 0; c < 255; ++c) {
        chars.push_back(is_word_char(c)
                            ? llvm::ConstantInt::getTrue(*context)
                            : llvm::ConstantInt::getFalse(*context));
      }

      llvm::ArrayType *table_type = llvm::ArrayType::get(i1_type, chars.size());
      llvm::Constant *table_data = llvm::ConstantArray::get(table_type, chars);

      word_lookup_table =
          new llvm::GlobalVariable(*module, table_type,
                                   true, // isConstant
                                   llvm::GlobalVariable::PrivateLinkage,
                                   table_data, "word_lookup_lookup");
    }

    std::vector<llvm::Type *> param_types = {
        ptr_type, i64_type, ptr_type,
        ptr_type}; // input string, out start, out end
    llvm::FunctionType *func_type =
        llvm::FunctionType::get(i8_type, param_types, false);

    function = llvm::Function::Create(
        func_type, llvm::Function::ExternalLinkage, "next_token", module.get());

    f_input_ptr = function->getArg(0);
    f_input_len = function->getArg(1);
    f_out_start = function->getArg(2);
    f_out_end = function->getArg(3);

    llvm::BasicBlock *entry_bb =
        llvm::BasicBlock::Create(*context, "entry", function);
    builder.SetInsertPoint(entry_bb);

    accepting_kind_alloc =
        builder.CreateAlloca(i8_type, nullptr, "last_accepting_kind");

    builder.CreateStore(builder.getInt8(0), accepting_kind_alloc);

    is_word_alloc = builder.CreateAlloca(i1_type, nullptr, "is_word_char");

    builder.CreateStore(llvm::ConstantInt::getFalse(*context), is_word_alloc);

    done = llvm::BasicBlock::Create(*context, "done", function);
    builder.SetInsertPoint(done);
    done_phi = builder.CreatePHI(i64_type, 0, "done_phi");

    for (auto &[_, s] : dfa) {
      setup_state(s, builder);
    }

    emit_ws_skip(entry_bb, dfa.at(start_state_id), builder);

    for (auto &[_, s] : dfa) {
      emit_state(s, builder);
    }

    builder.SetInsertPoint(done);

    llvm::Value *out =
        builder.CreateLoad(i8_type, accepting_kind_alloc, "token_kind_out");
    builder.CreateRet(out);

    if (llvm::verifyFunction(*function, &llvm::errs())) {
      std::cerr << "Function verification failed!\n";
      return;
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::FunctionPassManager FPM;
    FPM.addPass(llvm::PromotePass());
    FPM.addPass(llvm::SimplifyCFGPass());
    FPM.addPass(llvm::JumpThreadingPass());
    FPM.addPass(llvm::SCCPPass());
    FPM.addPass(llvm::GVNPass());

    llvm::ModulePassManager MPM =
        PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));

    MPM.run(*module, MAM);

    std::error_code EC;
    llvm::raw_fd_ostream dest(out_ir, EC);

    if (EC) {
      llvm::errs() << "Could not open file: " << EC.message() << "\n";
      return;
    }

    module->print(dest, nullptr);

    write_header(out_header);
  }

private:
  Cursor cursor;
  std::unique_ptr<llvm::LLVMContext> context;
  std::map<std::string, uint8_t> token_kind_map;
  llvm::Function *function;
  llvm::BasicBlock *done;
  llvm::PHINode *done_phi;
  llvm::Type *i1_type;
  llvm::IntegerType *i8_type;
  llvm::IntegerType *i64_type;
  llvm::Value *f_input_ptr;
  llvm::Value *f_input_len;
  llvm::Value *f_out_start;
  llvm::Value *f_out_end;
  llvm::AllocaInst *accepting_kind_alloc;
  llvm::AllocaInst *is_word_alloc;
  std::map<int, DFAState> dfa;
  llvm::GlobalVariable *word_lookup_table;

  void write_header(std::string filename) {

    std::ofstream headerFile(filename);

    headerFile << "#pragma once" << std::endl << std::endl;
    headerFile << "#include <stdint.h>" << std::endl;
    headerFile << "#include \"base.h\"" << std::endl;

    if (!headerFile.is_open()) {
      std::cerr << "Error: Could not open file " << filename << std::endl;
      return;
    }

    for (auto const &[k, v] : token_kind_map) {
      headerFile << "#define TOKEN_KIND_" << k << " " << static_cast<int>(v)
                 << std::endl;
    }

    headerFile << "int8_t next_token(const uint8_t *text, size_t len, uint8_t "
                  "*token_start, uint8_t *token_end);"
               << std::endl;
  }

  void emit_ws_skip(llvm::BasicBlock *entry_bb, DFAState &state,
                    llvm::IRBuilder<> &builder) {
    auto *pre_start = llvm::BasicBlock::Create(*context, "pre_start", function);
    auto *pre_body = llvm::BasicBlock::Create(*context, "pre_body", function);
    auto *skip_ws = llvm::BasicBlock::Create(*context, "skip_ws", function);
    auto *to_start = llvm::BasicBlock::Create(*context, "to_start", function);
    auto *pre_eof = llvm::BasicBlock::Create(*context, "pre_eof", function);

    builder.SetInsertPoint(entry_bb);
    builder.CreateBr(pre_start);

    builder.SetInsertPoint(pre_start);
    llvm::PHINode *ws_i = builder.CreatePHI(i64_type, 2, "ws_i");
    ws_i->addIncoming(builder.getInt64(0), entry_bb);
    llvm::Value *is_eof = builder.CreateICmpEQ(ws_i, f_input_len, "ws_is_eof");
    builder.CreateCondBr(is_eof, pre_eof, pre_body);

    builder.SetInsertPoint(pre_body);
    llvm::Value *c_ptr = builder.CreateGEP(i8_type, f_input_ptr, ws_i);
    llvm::Value *c = builder.CreateLoad(i8_type, c_ptr, "ws_c");
    llvm::SwitchInst *sw = builder.CreateSwitch(c, to_start, 4);
    sw->addCase(builder.getInt8(' '), skip_ws);
    sw->addCase(builder.getInt8('\t'), skip_ws);
    sw->addCase(builder.getInt8('\r'), skip_ws);
    sw->addCase(builder.getInt8('\n'), skip_ws);

    builder.SetInsertPoint(skip_ws);
    ws_i->addIncoming(builder.CreateAdd(ws_i, builder.getInt64(1), "ws_i.next"),
                      skip_ws);
    builder.CreateBr(pre_start);

    builder.SetInsertPoint(to_start);
    builder.CreateStore(ws_i, f_out_start);
    state.i_phi->addIncoming(ws_i, to_start);
    builder.CreateBr(state.bb);

    builder.SetInsertPoint(pre_eof);
    builder.CreateStore(builder.getInt8(TOKEN_KIND_EOF), accepting_kind_alloc);
    builder.CreateStore(ws_i, f_out_start);
    builder.CreateStore(ws_i, f_out_end);
    done_phi->addIncoming(ws_i, pre_eof);
    builder.CreateBr(done);
  }

  void setup_state(DFAState &state, llvm::IRBuilder<> &builder) {
    state.bb = llvm::BasicBlock::Create(
        *context, "state_start_" + std::to_string(state.id), function);

    builder.SetInsertPoint(state.bb);

    state.i_phi =
        builder.CreatePHI(i64_type, 0, "i.state_" + std::to_string(state.id));

    if (state.is_accepting) {
      uint8_t token_kind_id = token_kind_map.size() + USER_TOKEN_OFFSET;
      if (token_kind_map.contains(state.token_name)) {
        token_kind_id = token_kind_map[state.token_name];
      } else {
        token_kind_map[state.token_name] = token_kind_id;
      }

      builder.CreateStore(builder.getInt8(token_kind_id), accepting_kind_alloc);
      builder.CreateStore(state.i_phi, f_out_end);
    } // } else {
    //   builder.CreateStore(state.i_phi, f_out_start);
    // }

    state.body_bb = llvm::BasicBlock::Create(
        *context, "state_" + std::to_string(state.id), function);

    done_phi->addIncoming(state.i_phi, state.bb);
    llvm::Value *is_end =
        builder.CreateICmpEQ(state.i_phi, f_input_len, "is_end");
    builder.CreateCondBr(is_end, done, state.body_bb);
  }

  void emit_state(DFAState &state, llvm::IRBuilder<> &builder) {
    builder.SetInsertPoint(state.body_bb);

    llvm::Value *c_ptr = builder.CreateGEP(i8_type, f_input_ptr, state.i_phi);
    llvm::Value *c = builder.CreateLoad(i8_type, c_ptr, "c");

    llvm::Value *is_c_word =
        builder.CreateGEP(i1_type, word_lookup_table, c, "is_c_word_ptr");

    is_c_word = builder.CreateLoad(i1_type, is_c_word, "is_c_word");

    llvm::Value *is_last_word =
        builder.CreateLoad(i1_type, is_word_alloc, "is_last_word");

    llvm::BasicBlock *on_mismatch = llvm::BasicBlock::Create(
        *context, "mismatch_" + std::to_string(state.id), function);

    if (state.is_word_boundary) {
      llvm::Value *is_boundary =
          builder.CreateICmpNE(is_c_word, is_last_word, "word_boundary");

      llvm::BasicBlock *resume = llvm::BasicBlock::Create(
          *context, "resume" + std::to_string(state.id), function);
      builder.CreateCondBr(is_boundary, resume, on_mismatch);
      builder.SetInsertPoint(resume);
      state.body_bb = resume;
    }

    builder.CreateStore(is_c_word, is_word_alloc);

    state.i_next =
        builder.CreateAdd(state.i_phi, builder.getInt64(1), "i.next");

    builder.SetInsertPoint(on_mismatch);
    done_phi->addIncoming(state.i_phi, on_mismatch);
    builder.CreateBr(done);

    builder.SetInsertPoint(state.body_bb);

    llvm::SwitchInst *sw =
        builder.CreateSwitch(c, on_mismatch, state.transitions.size());

    for (const auto &[chr, id] : state.transitions) {
      DFAState &target = dfa.at(id);

      llvm::BasicBlock *case_bb =
          llvm::BasicBlock::Create(*context,
                                   "case_" + std::to_string(state.id) + "_to_" +
                                       std::to_string(target.id),
                                   function);

      builder.SetInsertPoint(case_bb);
      builder.CreateBr(target.bb);
      target.i_phi->addIncoming(state.i_next, case_bb);

      sw->addCase(builder.getInt8(chr), case_bb);
    }
  }

  Fragment build_nfa(std::shared_ptr<Node> node) {
    if (std::shared_ptr<Literal> literal =
            std::dynamic_pointer_cast<Literal>(node)) {
      std::shared_ptr<State> start = std::make_shared<State>();
      std::shared_ptr<State> end = std::make_shared<State>();
      if (literal->escaped) {
        switch (literal->ch) {
        case 'b':
          start->is_word_boundary = true;
          start->add_transition(std::nullopt, end);
          break;
        case 'n':
          start->add_transition('\n', end);
          break;
        case 'r':
          start->add_transition('\r', end);
          break;
        default:
          start->add_transition(literal->ch, end);
        }
      } else if (literal->ch == '.') {
        for (unsigned char c = 1; c < 127; ++c) {
          start->add_transition(c, end);
        }
      } else {
        start->add_transition(literal->ch, end);
      }
      return Fragment(start, {end});
    } else if (std::shared_ptr<Set> set =
                   std::dynamic_pointer_cast<Set>(node)) {
      std::shared_ptr<State> start = std::make_shared<State>();
      std::shared_ptr<State> end = std::make_shared<State>();
      for (char ch : set->chars) {
        start->add_transition(ch, end);
      }
      return Fragment(start, {end});
    } else if (std::shared_ptr<Concatenate> concat =
                   std::dynamic_pointer_cast<Concatenate>(node)) {
      Fragment left = build_nfa(concat->a);
      Fragment right = build_nfa(concat->b);
      for (std::shared_ptr<State> s : left.acceptings) {
        s->add_transition(std::nullopt, right.start);
      }
      return Fragment(left.start, right.acceptings);
    } else if (std::shared_ptr<Alternate> concat =
                   std::dynamic_pointer_cast<Alternate>(node)) {
      Fragment left = build_nfa(concat->a);
      Fragment right = build_nfa(concat->b);

      std::shared_ptr<State> start = std::make_shared<State>();
      std::shared_ptr<State> end = std::make_shared<State>();

      start->add_transition(std::nullopt, left.start);
      start->add_transition(std::nullopt, right.start);

      for (std::shared_ptr<State> s : left.acceptings) {
        s->add_transition(std::nullopt, end);
      }
      for (std::shared_ptr<State> s : right.acceptings) {
        s->add_transition(std::nullopt, end);
      }
      return Fragment(start, {end});
    } else if (std::shared_ptr<Star> star =
                   std::dynamic_pointer_cast<Star>(node)) {
      Fragment inner = build_nfa(star->inner);

      std::shared_ptr<State> start = std::make_shared<State>();
      std::shared_ptr<State> end = std::make_shared<State>();

      start->add_transition(std::nullopt, inner.start);
      start->add_transition(std::nullopt, end);

      for (std::shared_ptr<State> s : inner.acceptings) {
        s->add_transition(std::nullopt, inner.start);
        s->add_transition(std::nullopt, end);
      }
      return Fragment(start, {end});
    } else if (std::shared_ptr<Plus> plus =
                   std::dynamic_pointer_cast<Plus>(node)) {
      Fragment inner = build_nfa(plus->inner);

      std::shared_ptr<State> end = std::make_shared<State>();

      for (std::shared_ptr<State> s : inner.acceptings) {
        s->add_transition(std::nullopt, inner.start);
        s->add_transition(std::nullopt, end);
      }
      return Fragment(inner.start, {end});
    } else if (std::shared_ptr<Question> question =
                   std::dynamic_pointer_cast<Question>(node)) {
      Fragment inner = build_nfa(question->inner);

      std::shared_ptr<State> start = std::make_shared<State>();
      std::shared_ptr<State> end = std::make_shared<State>();

      start->add_transition(std::nullopt, inner.start);
      start->add_transition(std::nullopt, end);

      for (std::shared_ptr<State> s : inner.acceptings) {
        s->add_transition(std::nullopt, end);
      }
      return Fragment(start, {end});
    }
    throw std::runtime_error("Unexpected node");
  }

  std::shared_ptr<Node> atom() {
    if (cursor == '(') {
      ++cursor;
      std::shared_ptr<Node> n = expr();
      if (cursor != ')') {
        throw std::runtime_error("Expected )");
      }
      ++cursor;
      return n;
    } else if (cursor == '[') {
      ++cursor;
      std::shared_ptr<Set> n = std::make_unique<Set>();
      char from = 0;
      bool range = false, escaped = false;
      while (!cursor.eof() && cursor != ']') {
        if (range && from != 0) {
          for (char ch = from; ch < cursor.get(); ++ch) {
            n->chars.insert(ch);
          }
          from = 0;
        } else if (cursor == '\\') {
          if (escaped) {
            escaped = false;
            from = '\\';
          } else {
            escaped = true;
          }
        }
        if (!escaped && from != 0 && cursor == '-') {
          range = true;
        } else {
          if (from != 0) {
            n->chars.insert(from);
          }
          from = cursor.get();
        }
        ++cursor;
      }
      if (from != 0) {
        n->chars.insert(from);
      }
      if (cursor != ']') {
        throw std::runtime_error("Expected ]");
      }
      ++cursor;
      return n;
    } else if (cursor != '*' && cursor != '+' && cursor != '?') {
      bool escaped = false;
      if (cursor == '\\') {
        escaped = true;
        ++cursor;
      }
      char ch = cursor.get();
      ++cursor;
      return std::make_unique<Literal>(ch, escaped);
    }
    throw std::runtime_error("Unexpected char");
  }

  std::shared_ptr<Node> factor() {
    std::shared_ptr<Node> inner = atom();
    if (cursor.eof()) {
      return inner;
    }
    switch (cursor.get()) {
    case '*':
      ++cursor;
      return std::make_unique<Star>(std::move(inner));
    case '+':
      ++cursor;
      return std::make_unique<Plus>(std::move(inner));
    case '?':
      ++cursor;
      return std::make_unique<Question>(std::move(inner));
    }
    return inner;
  }

  std::shared_ptr<Node> term() {
    std::shared_ptr<Node> l = factor();
    while (!cursor.eof() && cursor != ')' && cursor != '|') {
      l = std::make_unique<Concatenate>(std::move(l), factor());
    }
    return l;
  }

  std::shared_ptr<Node> expr() {
    std::shared_ptr<Node> l = term();
    while (!cursor.eof() && cursor == '|') {
      ++cursor;
      l = std::make_unique<Alternate>(std::move(l), term());
    }
    return l;
  }
};

static std::vector<std::string> split_by_string(const std::string &str,
                                                const std::string &delimiter) {
  std::vector<std::string> tokens;
  size_t start = 0;
  size_t end = str.find(delimiter);

  while (end != std::string::npos) {
    tokens.push_back(str.substr(start, end - start));
    start = end + delimiter.length();
    end = str.find(delimiter, start);
  }

  tokens.push_back(str.substr(start));

  return tokens;
}

int main(int argc, char *argv[]) {

  if (argc != 4) {
    exit(1);
  }

  std::ifstream file(argv[1]);

  if (!file.is_open()) {
    std::cerr << "Error: Could not open file." << std::endl;
    return 1;
  }

  LexerBuilder lb;

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> parts = split_by_string(line, ": ");

    if (parts.size() != 2) {
      std::cerr << "Error: Could not read lexer definition." << std::endl;
      return 1;
    }

    std::cout << "Adding \"" << parts[0] << "\" *= \\" << parts[1] << "\\"
              << std::endl;
    lb.add(parts[0], parts[1]);
  }

  file.close();

  lb.build(argv[2], argv[3]);
}
