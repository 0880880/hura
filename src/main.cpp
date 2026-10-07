#include <algorithm>
#include <cstddef>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <ranges>
#include <set>
#include <string>
#include <utility>
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
#include "error.hpp"

struct Production {
  bool is_terminal;
  bool is_epsilon;
  std::string value;
  uint8_t token_kind;

  static Production terminal(uint8_t kind) { return {true, false, "", kind}; }

  static Production nonterminal(std::string name) {
    return {false, false, name, 0};
  }

  static Production epsilon() { return {false, true, "", 0}; }

  std::string
  to_string(const std::function<std::string(uint8_t)> &mapper) const {
    return is_epsilon ? "ε" : (is_terminal ? mapper(token_kind) : value);
  }

  constexpr bool operator==(const Production &other) const {
    return is_terminal == other.is_terminal && token_kind == other.token_kind &&
           value == other.value && is_epsilon == other.is_epsilon;
  }

  auto operator<=>(const Production &other) const {
    if (is_terminal != other.is_terminal) {
      return is_terminal <=> other.is_terminal;
    }
    if (is_epsilon != other.is_epsilon) {
      return is_epsilon <=> other.is_epsilon;
    }
    if (token_kind != other.token_kind) {
      return token_kind <=> other.token_kind;
    }
    return value <=> other.value;
  }
};

struct GrammarDefinition {
  std::string from;
  std::vector<Production> g;

  constexpr size_t real_size() const {
    return (g.size() == 1 && g[0].is_epsilon) ? 0 : g.size();
  }
};

struct LRItem {
  std::string from;
  std::vector<Production> prods;
  std::set<Production> lookaheads;
  size_t cursor = 0;

  constexpr bool eof(int offset) const {
    return cursor + offset == prods.size() ||
           (prods.size() == 1 && prods[0].is_epsilon);
  }

  constexpr bool eof() const { return eof(0); }

  constexpr Production get() const { return prods[cursor]; }

  constexpr size_t real_size() const {
    return (prods.size() == 1 && prods[0].is_epsilon) ? 0 : prods.size();
  }

  std::string
  to_string(const std::function<std::string(uint8_t)> &mapper) const {
    std::string gs;
    for (size_t i = 0; i < prods.size(); ++i) {
      const Production &prod = prods[i];
      if (i == cursor) {
        gs += "·";
      }
      gs += " ";
      gs += prod.to_string(mapper);
    }
    if (prods.size() == cursor) {
      gs += "·";
    }
    if (!lookaheads.empty()) {
      for (const Production &la : lookaheads) {
        gs += ", ";
        gs += la.to_string(mapper);
      }
    }
    return from + " -> " + gs;
  }

  constexpr bool lalr_equals(const LRItem &other) const {
    return from == other.from && prods == other.prods && cursor == other.cursor;
  }

  auto operator<=>(const LRItem &) const = default;
};

struct DFAState {
  std::set<LRItem> kernel;
  std::set<LRItem> closures;
  std::map<Production, int> transitions;
  std::string mode = "";
  bool worked_on = false;

  std::string to_string(const std::function<std::string(uint8_t)> &mapper,
                        bool escape = false) const {
    std::string newline = escape ? "\\n" : "\n";
    std::string sb;
    if (kernel.empty()) {
      return mode;
    }
    for (const LRItem &d : kernel) {
      sb += d.to_string(mapper);
      sb += newline;
    }
    if (!closures.empty()) {
      sb += "~~~~" + newline;
      for (const LRItem &d : closures) {
        sb += d.to_string(mapper);
        sb += newline;
      }
    }
    return sb;
  }

  auto operator<=>(const DFAState &) const = default;
};

struct LALRKeyItem {
  std::string from;
  std::vector<Production> prods;
  size_t cursor = 0;

  auto operator<=>(const LALRKeyItem &) const = default;
};

struct LALRKey {
  std::set<LALRKeyItem> kernel;
  std::set<LALRKeyItem> closures;
  bool worked_on = false;

  LALRKey(const DFAState &state) {
    for (const LRItem &i : state.kernel) {
      kernel.insert({i.from, i.prods, i.cursor});
    }
    for (const LRItem &i : state.closures) {
      closures.insert({i.from, i.prods, i.cursor});
    }
    worked_on = state.worked_on;
  }

  auto operator<=>(const LALRKey &) const = default;
};

struct StateIRData {
  llvm::BasicBlock *start_bb;
};

constexpr inline uint32_t pack(uint8_t shifted, uint16_t stack_index) {
  return (static_cast<uint32_t>(shifted) << 16) | stack_index;
}

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

struct Rule {
  std::string from;
  size_t pop_count;
};

class ParserBuilder {
private:
  std::vector<GrammarDefinition> grammarDefinitions;
  std::vector<Rule> rules;
  std::string start;
  std::deque<DFAState> states;
  std::deque<StateIRData> states_data;
  std::map<std::vector<Production>, std::set<Production>> first_table;

  std::map<std::string, uint8_t> nonterminal_id_map;
  std::map<std::string, uint8_t> token_id_map;
  std::map<uint8_t, std::string> id_token_map;

  std::vector<std::vector<int>> GOTO_table;

  std::map<size_t, llvm::BasicBlock *> reduce_bb_map;

  std::unique_ptr<llvm::Module> mod;
  std::unique_ptr<llvm::LLVMContext> context;
  std::map<std::string, uint8_t> token_kind_map;
  llvm::Function *function;
  llvm::IntegerType *i1_type;
  llvm::IntegerType *i8_type;
  llvm::IntegerType *i32_type;
  llvm::IntegerType *i64_type;
  llvm::PointerType *ptr_type;
  llvm::Type *void_type;
  llvm::StructType *node_type;
  llvm::StructType *storage_type;

  llvm::Value *stack_alloca;
  llvm::Value *nodes_alloca;
  llvm::Value *i_alloca;

  llvm::Value *fn_param_tokens;
  llvm::Value *fn_param_len;

  void reserve_storage(llvm::IRBuilder<> &builder, llvm::Type *inner_type,
                       llvm::Value *storage, size_t reserved) {
    llvm::Value *sp_ptr =
        builder.CreateStructGEP(storage_type, storage, 0, "get_ptr");
    llvm::Value *len_ptr =
        builder.CreateStructGEP(storage_type, storage, 1, "get_len");
    llvm::Value *cap_ptr =
        builder.CreateStructGEP(storage_type, storage, 2, "get_cap");

    llvm::FunctionCallee malloc_fn =
        mod->getOrInsertFunction("malloc", ptr_type, i64_type);
    const llvm::DataLayout &DL = mod->getDataLayout();

    llvm::Value *heap = builder.CreateCall(
        malloc_fn, llvm::ConstantInt::get(
                       i64_type, DL.getTypeAllocSize(inner_type) * reserved));
    builder.CreateStore(heap, sp_ptr);
    builder.CreateStore(llvm::ConstantInt::get(i64_type, reserved), cap_ptr);
    builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), len_ptr);
  }

  llvm::Value *back_storage(llvm::IRBuilder<> &builder, llvm::Type *inner_type,
                            llvm::Value *storage) {
    llvm::Value *sp_ptr =
        builder.CreateStructGEP(storage_type, storage, 0, "get_ptr");
    llvm::Value *len_ptr =
        builder.CreateStructGEP(storage_type, storage, 1, "get_len");
    llvm::Value *len = builder.CreateLoad(i64_type, len_ptr);

    llvm::Value *end =
        builder.CreateSub(len, llvm::ConstantInt::get(i64_type, 1));
    return builder.CreateGEP(inner_type, sp_ptr, end);
  }

  void push_storage(llvm::IRBuilder<> &builder, llvm::Type *inner_type,
                    llvm::Value *storage, llvm::Value *val) {
    llvm::Value *len_ptr =
        builder.CreateStructGEP(storage_type, storage, 1, "get_len");
    llvm::Value *len = builder.CreateLoad(i64_type, len_ptr);
    llvm::Value *cap_ptr =
        builder.CreateStructGEP(storage_type, storage, 2, "get_cap");
    llvm::Value *cap = builder.CreateLoad(i64_type, cap_ptr);
    llvm::Value *should_realloc = builder.CreateICmpUGE(len, cap);

    llvm::BasicBlock *realloc_br =
        llvm::BasicBlock::Create(*context, "realloc_br");
    llvm::BasicBlock *resume_br =
        llvm::BasicBlock::Create(*context, "resume_br");

    builder.CreateCondBr(should_realloc, realloc_br, resume_br);

    builder.SetInsertPoint(realloc_br);

    llvm::FunctionCallee realloc_fn =
        mod->getOrInsertFunction("realloc", ptr_type, ptr_type, i64_type);
    const llvm::DataLayout &DL = mod->getDataLayout();

    llvm::Value *sp_ptr =
        builder.CreateStructGEP(storage_type, storage, 0, "get_ptr");
    llvm::Value *sp = builder.CreateLoad(ptr_type, sp_ptr);
    llvm::Value *new_cap =
        builder.CreateShl(cap, llvm::ConstantInt::get(i64_type, 1));
    builder.CreateStore(new_cap, cap_ptr);
    llvm::Value *new_size = builder.CreateMul(
        new_cap,
        llvm::ConstantInt::get(i64_type, DL.getTypeAllocSize(inner_type)));
    llvm::Value *new_ptr =
        builder.CreateCall(realloc_fn, {sp, new_size}, "realloc_storage");
    builder.CreateStore(new_ptr, sp_ptr);
    builder.CreateBr(resume_br);
    builder.SetInsertPoint(resume_br);

    sp = builder.CreateLoad(ptr_type, sp_ptr);

    llvm::Value *back_ptr =
        builder.CreateGEP(inner_type, sp, len, "storage_back");
    builder.CreateStore(val, back_ptr);

    llvm::Value *new_len =
        builder.CreateAdd(len, llvm::ConstantInt::get(i64_type, 1));
    builder.CreateStore(new_len, len_ptr);
  }

  void pop_storage(llvm::IRBuilder<> &builder, llvm::Value *storage, int N) {
    llvm::Value *len_ptr =
        builder.CreateStructGEP(storage_type, storage, 1, "get_len");
    llvm::Value *len = builder.CreateLoad(i64_type, len_ptr);
    llvm::Value *new_len =
        builder.CreateSub(len, llvm::ConstantInt::get(i64_type, N));
    builder.CreateStore(new_len, len_ptr);
  }

  llvm::Value *peek_storage(llvm::IRBuilder<> &builder, llvm::Type *inner_type,
                            llvm::Value *storage) {
    llvm::Value *len_ptr =
        builder.CreateStructGEP(storage_type, storage, 1, "get_len");
    llvm::Value *len = builder.CreateLoad(i64_type, len_ptr);
    llvm::Value *top =
        builder.CreateSub(len, llvm::ConstantInt::get(i64_type, 1));
    llvm::Value *sp_ptr =
        builder.CreateStructGEP(storage_type, storage, 0, "get_ptr");
    llvm::Value *sp = builder.CreateLoad(ptr_type, sp_ptr);
    return builder.CreateGEP(inner_type, sp, top);
  }

  void free_storage(llvm::IRBuilder<> &builder, llvm::Value *storage) {
    llvm::FunctionCallee free_fn =
        mod->getOrInsertFunction("free", void_type, ptr_type);
    llvm::Value *sp_ptr =
        builder.CreateStructGEP(storage_type, storage, 0, "get_ptr");
    builder.CreateCall(free_fn, sp_ptr);
  }

  void emit_ir() {
    context = std::make_unique<llvm::LLVMContext>();

    mod = std::make_unique<llvm::Module>("lexer", *context);

    llvm::IRBuilder<> builder(*context);

    i1_type = llvm::Type::getInt1Ty(*context);
    i8_type = llvm::Type::getInt8Ty(*context);
    i32_type = llvm::Type::getInt32Ty(*context);
    i64_type = llvm::Type::getInt64Ty(*context);
    ptr_type = llvm::PointerType::get(*context, 0);
    void_type = llvm::Type::getVoidTy(*context);

    // Node {
    // ui8 kind;
    // ptr start;
    // ptr end;
    // ui8 children_count;
    // ui32 subtree_size;
    // }
    std::vector<llvm::Type *> node_body = {i8_type, ptr_type, ptr_type, i8_type,
                                           i32_type};
    node_type = llvm::StructType::create(*context, "Node");
    node_type->setBody(node_body);

    // Storage {
    // ptr storage;
    // ui64 len;
    // ui64 cap;
    // }
    std::vector<llvm::Type *> storage_body = {ptr_type, i64_type, i64_type};
    storage_type = llvm::StructType::create(*context, "Stack");
    storage_type->setBody(storage_body);

    std::vector<llvm::Type *> param_types = {ptr_type,
                                             i64_type}; // input tokens (i8*)
    llvm::FunctionType *func_type =
        llvm::FunctionType::get(ptr_type, param_types, false);

    function = llvm::Function::Create(
        func_type, llvm::Function::ExternalLinkage, "parse", mod.get());

    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(*context, "entry", function);

    builder.SetInsertPoint(entry);

    fn_param_tokens = function->getArg(0);
    fn_param_len = function->getArg(1);

    stack_alloca = builder.CreateAlloca(storage_type);
    nodes_alloca = builder.CreateAlloca(storage_type);

    i_alloca = builder.CreateAlloca(i64_type);
    builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), i_alloca);

    reserve_storage(builder, i32_type, stack_alloca, 100);
    reserve_storage(builder, node_type, nodes_alloca, 1000);

    // I'll use allocas let mem2reg turn them into phis

    for (size_t i = 0; i < states.size(); ++i) {
      const DFAState &state = states[i];
      setup_state(i, state, builder);
    }

    for (size_t i = 0; i < states.size(); ++i) {
      const DFAState &state = states[i];
      emit_state(i, state, builder);
    }

    free_storage(builder, stack_alloca);
    free_storage(builder, nodes_alloca);

    builder.CreateRet(llvm::ConstantPointerNull::get(ptr_type));

    if (llvm::verifyFunction(*function, &llvm::errs())) {
      std::cerr << "Function verification failed!\n";
      return;
    }

    // llvm::LoopAnalysisManager LAM;
    // llvm::FunctionAnalysisManager FAM;
    // llvm::CGSCCAnalysisManager CGAM;
    // llvm::ModuleAnalysisManager MAM;

    // llvm::PassBuilder PB;
    // PB.registerModuleAnalyses(MAM);
    // PB.registerCGSCCAnalyses(CGAM);
    // PB.registerFunctionAnalyses(FAM);
    // PB.registerLoopAnalyses(LAM);
    // PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    // llvm::FunctionPassManager FPM;
    // FPM.addPass(llvm::PromotePass());
    // FPM.addPass(llvm::SimplifyCFGPass());
    // FPM.addPass(llvm::JumpThreadingPass());
    // FPM.addPass(llvm::SCCPPass());
    // FPM.addPass(llvm::GVNPass());

    // llvm::ModulePassManager MPM =
    //     PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    // MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));

    // MPM.run(*module, MAM);

    std::error_code EC;
    llvm::raw_fd_ostream dest("parser.ll", EC);

    if (EC) {
      llvm::errs() << "Could not open file: " << EC.message() << "\n";
      return;
    }

    mod->print(dest, nullptr);
  }

  void setup_state(size_t idx, const DFAState &_, llvm::IRBuilder<> &builder) {

    StateIRData data;

    data.start_bb = llvm::BasicBlock::Create(
        *context, "state_start_" + std::to_string(idx), function);

    builder.SetInsertPoint(data.start_bb);

    states_data.push_back(data);
  }

  llvm::BasicBlock *get_reduce_bb(llvm::IRBuilder<> &builder,
                                  const LRItem &item, size_t rule_idx) {
    if (reduce_bb_map.contains(rule_idx)) {
      return reduce_bb_map[rule_idx];
    }
    llvm::BasicBlock *case_bb = llvm::BasicBlock::Create(
        *context, "case_reduce_" + item.from, function);

    auto saved = builder.saveIP();

    llvm::BasicBlock *error_handler =
        llvm::BasicBlock::Create(*context, "error", function);
    {

      builder.SetInsertPoint(error_handler);

      builder.CreateUnreachable();

      llvm::FunctionCallee throw_fn =
          mod->getOrInsertFunction("parser_throw", void_type, i8_type, i64_type,
                                   i8_type, ptr_type, i64_type);

      llvm::Value *i = builder.CreateLoad(i64_type, i_alloca);

      llvm::Value *kind_ptr = builder.CreateGEP(i8_type, fn_param_tokens, i);
      llvm::Value *kind = builder.CreateLoad(i8_type, kind_ptr);

      builder.CreateCall(
          throw_fn, {llvm::ConstantInt::get(
                         i8_type, static_cast<uint8_t>(ParserErrorKind::OTHER)),
                     i, kind, llvm::ConstantPointerNull::get(ptr_type),
                     llvm::ConstantInt::get(i64_type, 0)});

      free_storage(builder, stack_alloca);
      free_storage(builder, nodes_alloca);

      builder.CreateRet(llvm::ConstantPointerNull::get(ptr_type));
    }

    builder.SetInsertPoint(case_bb);
    // pop
    pop_storage(builder, stack_alloca, rules[rule_idx].pop_count);
    // push new state;

    llvm::Value *top = peek_storage(builder, i32_type, stack_alloca);

    int goto_cases = 0;

    for (const std::vector<int> &g : GOTO_table) {
      if (g[nonterminal_id_map[rules[rule_idx].from]] != -1) {
        ++goto_cases;
      }
    }

    llvm::Value *i = builder.CreateLoad(i64_type, i_alloca);

    llvm::Value *kind_ptr = builder.CreateGEP(i8_type, fn_param_tokens, i);

    {
      llvm::Value *len = builder.CreateLoad(
          i64_type, builder.CreateStructGEP(storage_type, nodes_alloca, 1),
          "nodes.len");
      llvm::Value *idx = len;
      for (size_t i = 0; i < rules[rule_idx].pop_count; ++i) {
        llvm::Value *peek = peek_storage(builder, node_type, nodes_alloca);
        llvm::Value *subtree_size = builder.CreateLoad(
            i32_type,
            builder.CreateStructGEP(node_type, peek, 4, "peek.subtree.ptr"));
        idx = builder.CreateSub(idx, subtree_size);
      }
      llvm::Value *parent = llvm::UndefValue::get(node_type);
      parent = builder.CreateInsertValue(
          parent,
          llvm::ConstantInt::get(
              i8_type, nonterminal_id_map[rules[rule_idx].from] + 128),
          0);
      parent = builder.CreateInsertValue(
          parent,
          builder.CreateSelect(
              llvm::ConstantInt::getBool(i1_type,
                                         rules[rule_idx].pop_count > 0),
              builder.CreateStructGEP(
                  node_type,
                  builder.CreateInBoundsGEP(
                      node_type,
                      builder.CreateLoad(
                          ptr_type, builder.CreateStructGEP(storage_type,
                                                            nodes_alloca, 0)),
                      idx),
                  1, "nodes[idx].start"),
              kind_ptr, "start.select"),
          1);
      parent = builder.CreateInsertValue(
          parent,
          builder.CreateSelect(
              llvm::ConstantInt::getBool(i1_type,
                                         rules[rule_idx].pop_count > 0),
              builder.CreateStructGEP(
                  node_type,
                  builder.CreateInBoundsGEP(
                      node_type,
                      builder.CreateLoad(
                          ptr_type, builder.CreateStructGEP(storage_type,
                                                            nodes_alloca, 0)),
                      builder.CreateSub(len,
                                        llvm::ConstantInt::get(i64_type, 1))),
                  2, "nodes[len-1].end"),
              kind_ptr, "end.select"),
          2);
      parent = builder.CreateInsertValue(
          parent, llvm::ConstantInt::get(i8_type, rules[rule_idx].pop_count),
          3);
      parent = builder.CreateInsertValue(
          parent,
          builder.CreateSub(
              builder.CreateAdd(len, llvm::ConstantInt::get(i32_type, 1)), idx),
          2);
    }

    llvm::SwitchInst *goto_sw =
        builder.CreateSwitch(top, error_handler, goto_cases);

    for (size_t goto_state_idx = 0; goto_state_idx < GOTO_table.size();
         ++goto_state_idx) {
      const std::vector<int> &g = GOTO_table[goto_state_idx];
      int dest = g[nonterminal_id_map[rules[rule_idx].from]];

      if (dest != -1) {
        llvm::BasicBlock *goto_case_bb = llvm::BasicBlock::Create(
            *context, "goto_case_" + std::to_string(dest), function);

        builder.SetInsertPoint(goto_case_bb);

        push_storage(builder, i32_type, stack_alloca,
                     llvm::ConstantInt::get(i32_type, dest));

        builder.CreateBr(states_data[dest].start_bb);

        goto_sw->addCase(
            llvm::ConstantInt::get(i32_type, goto_state_idx, false, true),
            goto_case_bb);
      }
    }

    builder.restoreIP(saved);

    return reduce_bb_map[rule_idx] = case_bb;
  }

  void emit_state(size_t idx, const DFAState &state,
                  llvm::IRBuilder<> &builder) {

    llvm::Value *i = builder.CreateLoad(i64_type, i_alloca);

    llvm::Value *kind_ptr = builder.CreateGEP(i8_type, fn_param_tokens, i);
    llvm::Value *kind = builder.CreateLoad(i8_type, kind_ptr);

    int num_cases = 0;

    for (auto const &[k, v] : state.transitions) {
      if (!k.is_terminal) {
        continue;
      }
      ++num_cases;
    }

    for (const LRItem &item : state.kernel) {
      if (!item.eof()) {
        continue;
      }
      num_cases += item.lookaheads.size();
    }

    for (const LRItem &item : state.closures) {
      if (!item.eof()) {
        continue;
      }
      num_cases += item.lookaheads.size();
    }

    llvm::BasicBlock *error_handler =
        llvm::BasicBlock::Create(*context, "error", function);
    {

      builder.SetInsertPoint(error_handler);

      std::vector<uint8_t> expected(state.transitions.size());
      int idx = 0;
      for (auto const &[k, v] : state.transitions) {
        if (k.is_epsilon) {
          continue;
        }
        expected[idx++] =
            k.is_terminal ? k.token_kind : nonterminal_id_map[k.value];
      }

      llvm::Constant *init = llvm::ConstantDataArray::get(*context, expected);

      auto *set_global = new llvm::GlobalVariable(
          *mod, init->getType(),
          /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage, init,
          "expected_" + std::to_string(idx));

      llvm::FunctionCallee throw_fn =
          mod->getOrInsertFunction("parser_throw", void_type, i8_type, i64_type,
                                   i8_type, ptr_type, i64_type);

      llvm::Value *i = builder.CreateLoad(i64_type, i_alloca);

      llvm::Value *kind_ptr = builder.CreateGEP(i8_type, fn_param_tokens, i);
      llvm::Value *kind = builder.CreateLoad(i8_type, kind_ptr);

      builder.CreateCall(
          throw_fn,
          {llvm::ConstantInt::get(
               i8_type, static_cast<uint8_t>(ParserErrorKind::UNEXPECTED)),
           i, kind, set_global,
           llvm::ConstantInt::get(i64_type, state.transitions.size())});

      free_storage(builder, stack_alloca);
      free_storage(builder, nodes_alloca);

      builder.CreateRet(llvm::ConstantPointerNull::get(ptr_type));
    }

    builder.SetInsertPoint(states_data[idx].start_bb);

    llvm::SwitchInst *sw = builder.CreateSwitch(kind, error_handler, num_cases);

    for (auto const &[k, v] : state.transitions) {
      if (!k.is_terminal) {
        continue;
      }
      llvm::BasicBlock *case_bb = llvm::BasicBlock::Create(
          *context, "case_shift_" + k.to_string([this](uint8_t id) {
            return this->map_id(id);
          }) + "_to_" + std::to_string(v),
          function);

      builder.SetInsertPoint(case_bb);
      push_storage(builder, i32_type, stack_alloca,
                   llvm::ConstantInt::get(i32_type, v, false));
      llvm::Value *increment =
          builder.CreateAdd(i, llvm::ConstantInt::get(i64_type, 1));
      builder.CreateStore(increment, i_alloca);

      llvm::Value *node = llvm::UndefValue::get(node_type);
      node = builder.CreateInsertValue(
          node, llvm::ConstantInt::get(i8_type, k.token_kind), 0,
          "node.insert.kind");
      node = builder.CreateInsertValue(node, kind_ptr, 1, "node.insert.start");
      node = builder.CreateInsertValue(
          node, builder.CreateGEP(i8_type, fn_param_tokens, increment), 2,
          "node.insert.end");
      node = builder.CreateInsertValue(node, llvm::ConstantInt::get(i8_type, 0),
                                       3, "node.insert.children_count");
      node =
          builder.CreateInsertValue(node, llvm::ConstantInt::get(i32_type, 1),
                                    4, "node.insert.subtree_size");
      push_storage(builder, node_type, nodes_alloca, node);
      builder.CreateBr(states_data[v].start_bb);
      sw->addCase(llvm::ConstantInt::get(i8_type, k.token_kind, false, true),
                  case_bb);
    }

    std::vector<LRItem> reduce_cases;
    reduce_cases.reserve(state.kernel.size() + state.closures.size());
    reduce_cases.insert(reduce_cases.end(), state.kernel.begin(),
                        state.kernel.end());
    reduce_cases.insert(reduce_cases.end(), state.closures.begin(),
                        state.closures.end());

    for (const LRItem &item : reduce_cases) {
      if (!item.eof()) {
        continue;
      }
      if (item.from == start) {
        llvm::BasicBlock *case_bb =
            llvm::BasicBlock::Create(*context, "case_accept", function);
        builder.SetInsertPoint(case_bb);

        builder.CreateRet(builder.CreateLoad(storage_type, nodes_alloca));

        sw->addCase(
            llvm::ConstantInt::get(i8_type, TOKEN_KIND_EOF, false, true),
            case_bb);
        continue;
      }
      for (const Production &p : item.lookaheads) {
        sw->addCase(llvm::ConstantInt::get(i8_type, p.token_kind, false, true),
                    get_reduce_bb(builder, item, find_rule(item)));
      }
    }
  }

  std::vector<GrammarDefinition> find(const std::string name) {
    std::vector<GrammarDefinition> out;
    out.reserve(1);
    for (GrammarDefinition &def : grammarDefinitions) {
      if (def.from == name) {
        out.push_back(def);
      }
    }
    return out;
  }

  void closure(const LRItem &i, std::set<LRItem> &out) {
    if (i.eof() || i.get().is_epsilon || i.get().is_terminal) {
      return;
    }
    std::vector<Production> follow_ups(i.prods.begin() + i.cursor + 1,
                                       i.prods.end());
    std::set<Production> lookaheads = FIRST(follow_ups);
    if (lookaheads.contains(Production::epsilon())) {
      lookaheads.erase(Production::epsilon());
      lookaheads.insert(i.lookaheads.begin(), i.lookaheads.end());
    }
    for (const GrammarDefinition &d : find(i.get().value)) {
      out.insert(LRItem{d.from, d.g, lookaheads});
    }
  }

  template <std::ranges::input_range R>
    requires std::same_as<std::ranges::range_value_t<R>, LRItem>
  void closure(R &&r, std::set<LRItem> &out) {
    size_t last = out.size() + 1;
    while (last != out.size()) {
      last = out.size();
      for (const LRItem &def : r) {
        closure(def, out);
      }
      for (const LRItem &def : out) {
        closure(def, out);
      }
    }
  }

  std::set<Production> FIRST(std::vector<Production> productions) {
    std::set<Production> result;
    for (const Production &p : productions) {
      const std::set<Production> &pf = first_table[std::vector<Production>{p}];
      bool has_epsilon = false;
      for (const Production &op : pf) {
        if (op.is_epsilon) {
          has_epsilon = true;
          continue;
        }
        result.insert(op);
      }
      if (!has_epsilon) {
        return result;
      }
    }
    result.insert(Production::epsilon());
    return result;
  }

  DFAState &GOTO(DFAState &state, const Production X) {
    DFAState next;
    std::vector<LRItem> dotBefore;
    dotBefore.reserve(state.kernel.size() + state.closures.size());
    for (const LRItem &i : state.kernel) {
      if (i.eof() || i.get() != X) {
        continue;
      }
      dotBefore.push_back(LRItem{i});
    }
    for (const LRItem &i : state.closures) {
      if (i.eof() || i.get() != X) {
        continue;
      }
      dotBefore.push_back(LRItem{i});
    }
    std::set<LRItem> kernel;
    std::set<LRItem> closures;
    for (LRItem &i : dotBefore) {
      ++i.cursor;
      kernel.insert(i);
      closure(i, closures);
    }
    for (const LRItem &i : kernel) {
      LRItem ni = LRItem{i};
      next.kernel.insert(ni);
    }
    for (const LRItem &i : closures) {
      LRItem ni = LRItem{i};
      next.closures.insert(ni);
    }
    int idx = -1;
    for (size_t j = 0; j < states.size(); ++j) {
      if (states[j].kernel == next.kernel) {
        idx = j;
        break;
      }
    }
    if (idx == -1) {
      idx = states.size();
      states.push_back(next);
    }
    state.transitions[X] = idx;
    return states[idx];
  }

  void build_transitions(DFAState &state) {
    std::set<Production> covered_items;
    for (const LRItem &i : state.kernel) {
      if (i.eof() || i.get().is_epsilon || covered_items.contains(i.get())) {
        continue;
      }
      covered_items.insert(i.get());
      DFAState &ns = GOTO(state, i.get());
      closure(ns.kernel, ns.closures);
    }
    for (const LRItem &i : state.closures) {
      if (i.eof() || i.get().is_epsilon || covered_items.contains(i.get())) {
        continue;
      }
      covered_items.insert(i.get());
      DFAState &ns = GOTO(state, i.get());
      closure(ns.kernel, ns.closures);
    }
  }

  size_t find_rule(const LRItem &item) {
    for (size_t i = 0; i < grammarDefinitions.size(); ++i) {
      const GrammarDefinition &def = grammarDefinitions[i];
      if (def.from == item.from && def.g == item.prods) {
        return i;
      }
    }
    throw std::runtime_error("Unknown rule");
  }

public:
  void add(const GrammarDefinition def) {
    grammarDefinitions.push_back(GrammarDefinition(def));
    rules.push_back(Rule{def.from, def.real_size()});
    if (!nonterminal_id_map.contains(def.from)) {
      nonterminal_id_map[def.from] = nonterminal_id_map.size();
    }
  }

  void set_start(const std::string start) { this->start = start; }

  void augment() {
    GrammarDefinition def;
    def.from = start + "_p";
    def.g.emplace_back(false, false, start, 0);
    add(def);
    start = start + "_p";
  }

  void build(bool LALR1 = true) {
    {
      bool changed = true;
      while (changed) {
        changed = false;
        for (const GrammarDefinition &def : grammarDefinitions) {
          bool natural_end = true;
          for (const Production &p : def.g) {
            std::set<Production> &pf = first_table[std::vector<Production>{p}];
            if (p.is_terminal) {
              size_t last_size = pf.size();
              pf.insert(p);
              if (last_size != pf.size()) {
                changed = true;
              }
            }
            if (p.is_epsilon) {
              size_t last_size = pf.size();
              pf.insert(Production::epsilon());
              if (last_size != pf.size()) {
                changed = true;
              }
            }
            bool has_epsilon = false;
            for (const Production &op : pf) {
              if (op.is_epsilon) {
                has_epsilon = true;
                continue;
              }
              std::set<Production> &s =
                  first_table[{Production{false, false, def.from, 0}}];
              size_t last_size = s.size();
              s.insert(op);
              if (last_size != s.size()) {
                changed = true;
              }
            }
            if (!has_epsilon) {
              natural_end = false;
              break;
            }
          }
          if (natural_end) {
            std::set<Production> &s =
                first_table[{Production{false, false, def.from, 0}}];
            size_t last_size = s.size();
            s.insert(Production::epsilon());
            if (last_size != s.size()) {
              changed = true;
            }
          }
        }
      }
    }

    states.push_back({});
    DFAState &state = states.back();
    for (const GrammarDefinition &def : find(start)) {
      state.kernel.insert(
          LRItem{def.from, def.g, {Production::terminal(TOKEN_KIND_EOF)}});
    }
    closure(state.kernel, state.closures);
    bool found_something = true;
    while (found_something) {
      found_something = false;
      for (size_t i = 0; i < states.size(); ++i) {
        DFAState &S = states[i];
        if (!S.worked_on) {
          S.worked_on = true;
          found_something = true;
          build_transitions(S);
        }
      }
    }

    create_GOTO();

    if (LALR1) {
      lalr1();
    }

    emit_ir();
  }

  void lalr1() {
    std::map<LALRKey, std::vector<size_t>> LALR1_groups;

    for (size_t i = 0; i < states.size(); ++i) {
      LALR1_groups[LALRKey{states[i]}].push_back(i);
    }

    std::vector<size_t> old_index_map(states.size());

    std::deque<DFAState> LALR1_states;
    std::deque<size_t> original_indices;
    for (auto &[_, group] : LALR1_groups) {
      if (group.empty()) {
        continue;
      }
      const DFAState &first = states[group[0]];
      DFAState lalr1 = {};
      lalr1.kernel = first.kernel;
      lalr1.closures = first.closures;
      lalr1.transitions = first.transitions;
      lalr1.worked_on = first.worked_on;
      size_t original_index = states.size();

      for (size_t state_idx : group) {
        const DFAState &S = states[state_idx];
        original_index = std::min(original_index, state_idx);
        old_index_map[state_idx] = LALR1_states.size();

        for (const auto &[sym, target_idx] : S.transitions) {
          lalr1.transitions[sym] = target_idx;
        }

        for (LRItem k : S.kernel) {
          auto it = std::find_if(
              lalr1.kernel.begin(), lalr1.kernel.end(),
              [&](const LRItem &item) { return item.lalr_equals(k); });
          LRItem i = *it;
          i.lookaheads.insert(k.lookaheads.begin(), k.lookaheads.end());
          lalr1.kernel.erase(it);
          lalr1.kernel.insert(i);
        }
        for (LRItem k : S.closures) {
          auto it = std::find_if(
              lalr1.closures.begin(), lalr1.closures.end(),
              [&](const LRItem &item) { return item.lalr_equals(k); });
          LRItem i = *it;
          i.lookaheads.insert(k.lookaheads.begin(), k.lookaheads.end());
          lalr1.closures.erase(it);
          lalr1.closures.insert(i);
        }
      }
      LALR1_states.push_back(lalr1);
      original_indices.push_back(original_index);
    }

    std::vector<size_t> indices(LALR1_states.size());
    std::iota(indices.begin(), indices.end(), 0);

    std::sort(indices.begin(), indices.end(), [&](size_t i, size_t j) {
      return original_indices[i] < original_indices[j];
    });

    std::vector<size_t> pre_to_final(LALR1_states.size());
    for (size_t i = 0; i < indices.size(); ++i) {
      pre_to_final[indices[i]] = i;
    }

    std::vector<size_t> old_to_final(states.size());
    for (size_t i = 0; i < old_to_final.size(); ++i) {
      old_to_final[i] = pre_to_final[old_index_map[i]];
    }

    std::deque<DFAState> sorted_LALR1(LALR1_states.size());
    for (size_t i = 0; i < LALR1_states.size(); ++i) {
      sorted_LALR1[i] = std::move(LALR1_states[indices[i]]);
    }

    states = std::move(sorted_LALR1);

    for (DFAState &S : states) {
      for (auto const &[p, i] : S.transitions) {
        S.transitions[p] = old_to_final[i];
      }
    }
  }

  void write_mermaid(const std::string out = "parser.mmd") {
    std::ofstream ofs(out);
    ofs << "flowchart TD" << std::endl;

    for (size_t i = 0; i < states.size(); ++i) {
      const DFAState &S = states[i];
      ofs << "I" << i << "(I" << i << "\\n"
          << S.to_string([this](uint8_t id) { return this->map_id(id); }, true)
          << ")" << std::endl;
      for (auto const &[k, v] : S.transitions) {
        ofs << "I" << i << "(I" << i << "\\n"
            << S.to_string([this](uint8_t id) { return this->map_id(id); },
                           true)
            << ") -->|"
            << k.to_string([this](uint8_t id) { return this->map_id(id); })
            << "| I" << v << std::endl;
      }
    }

    ofs.close();
  }

  void load_lexer_definition(const std::string source = "language.tex") {

    std::ifstream file(source);

    if (!file.is_open()) {
      std::cerr << "Error: Could not open file." << std::endl;
      return;
    }

    std::string line;
    while (std::getline(file, line)) {
      if (line.empty()) {
        continue;
      }

      std::vector<std::string> parts = split_by_string(line, ": ");

      if (parts.size() != 2) {
        std::cerr << "Error: Could not read lexer definition." << std::endl;
        return;
      }

      std::cout << "Adding \"" << parts[0] << "\" *= \\" << parts[1] << "\\"
                << std::endl;
      token_id_map[parts[0]] = token_id_map.size() + 2;
      id_token_map[id_token_map.size() + 2] = parts[0];
    }

    token_id_map["$"] = TOKEN_KIND_EOF;
    id_token_map[TOKEN_KIND_EOF] = "$";

    file.close();
  }

  uint8_t map_token(const std::string &token_name) {
    return token_id_map[token_name];
  }

  std::string map_id(uint8_t token_id) { return id_token_map[token_id]; }

  void validate() {
    std::vector<std::vector<std::string>> ACTION_table(states.size());

    for (size_t i = 0; i < states.size(); ++i) {
      ACTION_table[i].resize(token_id_map.size() + 1);
      for (auto const &[k, v] : states[i].transitions) {
        if (k.is_terminal) {
          ACTION_table[i][k.token_kind] = "s" + std::to_string(v);
        }
      }
      for (const LRItem &item : states[i].kernel) {
        if (item.eof()) {
          if (item.from == start) {
            ACTION_table[i][TOKEN_KIND_EOF] = "a";
            continue;
          }
          for (const Production &p : item.lookaheads) {
            if (ACTION_table[i][p.token_kind] != "" &&
                ACTION_table[i][p.token_kind] !=
                    ("r" + std::to_string(find_rule(item)))) {
              throw std::runtime_error("ACTION table conflic at (" +
                                       std::to_string(i) + ", " +
                                       std::to_string(p.token_kind) + ")");
            }
            ACTION_table[i][p.token_kind] =
                ("r" + std::to_string(find_rule(item)));
          }
        }
      }
      for (const LRItem &item : states[i].closures) {
        if (item.eof()) {
          for (const Production &p : item.lookaheads) {
            if (ACTION_table[i][p.token_kind] != "" &&
                ACTION_table[i][p.token_kind] !=
                    ("r" + std::to_string(find_rule(item)))) {
              throw std::runtime_error("ACTION table conflic at (" +
                                       std::to_string(i) + ", " +
                                       std::to_string(p.token_kind) + ")");
            }
            ACTION_table[i][p.token_kind] =
                ("r" + std::to_string(find_rule(item)));
          }
        }
      }
    }
  }

  void create_GOTO() {
    GOTO_table.resize(states.size());

    for (size_t i = 0; i < states.size(); ++i) {
      GOTO_table[i].resize(nonterminal_id_map.size());
      for (size_t j = 0; j < nonterminal_id_map.size(); ++j) {
        GOTO_table[i][j] = -1;
      }
      for (auto const &[k, v] : states[i].transitions) {
        if (!k.is_terminal) {
          GOTO_table[i][nonterminal_id_map[k.value]] = v;
        }
      }
    }
  }

  void simulate(std::vector<uint8_t> tokens) {

    std::vector<std::vector<std::pair<char, uint32_t>>> ACTION_table(
        states.size());

    for (size_t i = 0; i < states.size(); ++i) {
      ACTION_table[i].resize(token_id_map.size() + 1);
      for (auto const &[k, v] : states[i].transitions) {
        if (k.is_terminal) {
          ACTION_table[i][k.token_kind] = std::make_pair('s', v);
        }
      }
      for (const LRItem &item : states[i].kernel) {
        if (item.eof()) {
          if (item.from == start) {
            ACTION_table[i][TOKEN_KIND_EOF] = std::make_pair('a', 0);
            continue;
          }
          for (const Production &p : item.lookaheads) {
            if (ACTION_table[i][p.token_kind] != std::pair<char, uint32_t>{} &&
                ACTION_table[i][p.token_kind] !=
                    std::make_pair('r', find_rule(item))) {
              throw std::runtime_error("ACTION table conflic at (" +
                                       std::to_string(i) + ", " +
                                       std::to_string(p.token_kind) + ")");
            }
            ACTION_table[i][p.token_kind] =
                std::make_pair('r', find_rule(item));
          }
        }
      }
      for (const LRItem &item : states[i].closures) {
        if (item.eof()) {
          for (const Production &p : item.lookaheads) {
            if (ACTION_table[i][p.token_kind] != std::pair<char, uint32_t>{} &&
                ACTION_table[i][p.token_kind] !=
                    std::make_pair('r', find_rule(item))) {
              throw std::runtime_error("ACTION table conflic at (" +
                                       std::to_string(i) + ", " +
                                       std::to_string(p.token_kind) + ")");
            }
            ACTION_table[i][p.token_kind] =
                std::make_pair('r', find_rule(item));
          }
        }
      }
    }

    std::cout << "| State |";
    for (auto const &[k, v] : token_id_map) {
      std::cout << " " << k << " |";
    }
    std::cout << std::endl << "|---|";
    for (size_t ignored = 0; ignored < token_id_map.size(); ++ignored) {
      std::cout << "---|";
    }
    std::cout << std::endl;

    for (size_t i = 0; i < states.size(); ++i) {
      std::cout << "| " << i << " |";
      for (auto const &[k, v] : token_id_map) {
        if (ACTION_table[i][v] != std::pair<char, uint32_t>{}) {
          std::pair<char, uint32_t> act = ACTION_table[i][v];
          std::cout << " " << act.first << act.second << " |";
        } else {
          std::cout << "  |";
        }
      }
      std::cout << std::endl;
    }

    std::cout << "| State |";
    for (auto const &[k, v] : nonterminal_id_map) {
      std::cout << " " << k << " |";
    }
    std::cout << std::endl << "|---|";
    for (size_t ignored = 0; ignored < nonterminal_id_map.size(); ++ignored) {
      std::cout << "---|";
    }
    std::cout << std::endl;

    for (size_t i = 0; i < states.size(); ++i) {
      std::cout << "| " << i << " |";
      for (auto const &[k, v] : nonterminal_id_map) {
        if (GOTO_table[i][v] != -1) {
          int gt = GOTO_table[i][v];
          std::cout << " " << gt << " |";
        } else {
          std::cout << "  |";
        }
      }
      std::cout << std::endl;
    }

    int i = 0;
    std::vector<int> stack = {0};

    while (true) {
      int state = stack.back();
      std::cout << "i=" << i << ":" << id_token_map[tokens[i]]
                << "   state=" << state;
      auto [cmd, arg] = ACTION_table[state][tokens[i]];
      std::cout << " cmd=" << cmd << std::endl;
      switch (cmd) {
      case 's':
        stack.push_back(arg);
        ++i;
        continue;
      case 'r':
        for (size_t i = 0; i < rules[arg].pop_count; ++i) {
          stack.pop_back();
        }
        stack.push_back(
            GOTO_table[stack.back()][nonterminal_id_map[rules[arg].from]]);
        std::cout << "Reduced " << rules[arg].from << std::endl;
        continue;
      case 'a':
        std::cout << "Accepted" << std::endl;
        return;
      }
    }

    // Simulate
  }
};

int main() {

  ParserBuilder pb;

  pb.load_lexer_definition("parser_test.lex");

  {
    GrammarDefinition def;
    def.from = "A";
    def.g.push_back(Production::nonterminal("A"));
    def.g.push_back(Production::nonterminal("B"));
    def.g.push_back(Production::nonterminal("B"));
    pb.add(def);
  }
  {
    GrammarDefinition def;
    def.from = "A";
    def.g.push_back(Production::nonterminal("B"));
    pb.add(def);
  }
  {
    GrammarDefinition def;
    def.from = "B";
    def.g.push_back(Production::terminal(pb.map_token("1")));
    def.g.push_back(Production::nonterminal("C"));
    pb.add(def);
  }
  {
    GrammarDefinition def;
    def.from = "C";
    def.g.push_back(Production::epsilon());
    pb.add(def);
  }
  {
    GrammarDefinition def;
    def.from = "C";
    def.g.push_back(Production::terminal(pb.map_token("0")));
    def.g.push_back(Production::nonterminal("D"));
    pb.add(def);
  }
  {
    GrammarDefinition def;
    def.from = "D";
    def.g.push_back(Production::epsilon());
    pb.add(def);
  }
  {
    GrammarDefinition def;
    def.from = "D";
    def.g.push_back(Production::terminal(pb.map_token("0")));
    pb.add(def);
  }
  pb.set_start("A");
  pb.augment();
  pb.build();

  pb.write_mermaid();

  // pb.simulate({3, 2, 3, 3, 2, 2, TOKEN_KIND_EOF});

  return 0;
}