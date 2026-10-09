project(
  'hura',
  'c',
  'cpp',
  version: '0.1',
  meson_version: '>= 1.1',
  default_options: [
    'c_std=c11',
    'cpp_std=c++23',
    'warning_level=2',
  ],
)

inc = include_directories('include')
gen_inc = include_directories('.')

# ---------------------------------------------------------------------------
# LLVM
# ---------------------------------------------------------------------------
llvm_dep = dependency(
  'llvm',
  version: '>= 17',
  modules: ['core', 'support', 'irreader'],
  static: false,
)

llvm_inc_link = llvm_dep.partial_dependency(
  includes: true,
  link_args: true,
  links: true,
  compile_args: false,
)

llvm_args = [
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-D__STDC_LIMIT_MACROS',
]

vulkan_dep = dependency('vulkan', required: get_option('vulkan'))

lexer_builder = executable(
  'lexer_builder',
  'tools/lexer_builder.cpp',
  include_directories: inc,
  dependencies: llvm_inc_link,
  cpp_args: llvm_args,
  native: true,
)

parser_builder = executable(
  'parser_builder',
  'tools/parser_builder.cpp',
  include_directories: inc,
  dependencies: llvm_inc_link,
  cpp_args: llvm_args,
  native: true,
)

gen_header_dir = meson.project_source_root() / 'include'

lexer_gen = custom_target(
  'lexer_gen',
  input: 'specs/lexer.def',
  output: 'lexer.ll',
  command: [lexer_builder, '@INPUT@', gen_header_dir / 'lexer.h', '@OUTPUT@'],
  depend_files: files('include/base.h'),
)

parser_gen = custom_target(
  'parser_gen',
  input: ['specs/lexer.def', 'specs/parser.def'],
  output: 'parser.ll',
  command: [
    parser_builder,
    '@INPUT0@',
    '@INPUT1@',
    gen_header_dir / 'parser.h',
    '@OUTPUT@',
  ],
  depend_files: files('include/base.h', 'include/error.hpp', 'src/error.cpp'),
)

proj_dep = declare_dependency(
  include_directories: [inc, gen_inc],
  sources: [lexer_gen, parser_gen],
)

app_sources = files(
  'src/error.cpp',
  'src/main.cpp',
)

app_deps = [proj_dep, llvm_inc_link]
app_args = llvm_args

if vulkan_dep.found()
  app_deps += vulkan_dep
  app_args += '-DUSE_VULKAN=1'
endif

executable(
  'app',
  app_sources,
  lexer_gen[0],
  parser_gen[0],
  dependencies: app_deps,
  cpp_args: app_args,
)
