# Codegen tools: turn the specs/ definitions into C headers + LLVM IR,
# then compile the IR into objects. The result is exported as codegen_dep.

lexer_builder = executable('lexer_builder',
  'lexer_builder.cpp',
  dependencies : [llvm_dep])

parser_builder = executable('parser_builder',
  'parser_builder.cpp',
  include_directories : include_directories('../src'),
  dependencies : [llvm_dep])

lexer_spec = files(join_paths(meson.project_source_root(), 'specs', 'lexer.def'))
grammar_spec = files(join_paths(meson.project_source_root(), 'specs', 'grammar.def'))

# argv contract:
#   lexer_builder  <lexer def> <out lexer.h> <out lexer.ll>
lexer_gen = custom_target('lexer_gen',
  input : lexer_spec,
  output : ['lexer.h', 'lexer.ll'],
  command : [lexer_builder, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'])

# argv contract:
#   parser_builder <lexer def for token map> <grammar def> <out parser.h> <out parser.ll> <out parser.mmd>
# specs/lexer.def provides the token map; grammar.def uses terminals defined there.
parser_gen = custom_target('parser_gen',
  input : [lexer_spec, grammar_spec],
  output : ['parser.h', 'parser.ll', 'parser.mmd'],
  command : [parser_builder, '@INPUT0@', '@INPUT1@',
             '@OUTPUT0@', '@OUTPUT1@', '@OUTPUT2@'])

# compile the generated .ll files into objects
clang = find_program('clang')

ir_inputs = { 'lexer': lexer_gen[1], 'parser': parser_gen[1] }
ir_objs = []
foreach name, src : ir_inputs
  ir_objs += custom_target(name + '_ir',
    input : src,
    output : name + '.o',
    command : [clang, '-x', 'ir', '@INPUT@', '-c', '-o', '@OUTPUT@'])
endforeach

codegen_dep = declare_dependency(
  sources : [lexer_gen[0], parser_gen[0]] + ir_objs,
  include_directories : include_directories('../src'))
