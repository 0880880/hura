project('hura', 'c', 'cpp',
  version: '0.1',
  meson_version: '>= 1.1',
  default_options: [
    'c_std=c11',
    'cpp_std=c++23',
    'warning_level=2',
  ],
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
inc     = include_directories('include')  # base.h, error.hpp, ...
gen_inc = include_directories('.')        # build dir: generated lexer.h / parser.h

# ---------------------------------------------------------------------------
# LLVM
# ---------------------------------------------------------------------------
llvm_dep = dependency('llvm',
  version: '>= 17',
  modules: ['core', 'support', 'irreader'],   # adjust to what you use
  static: false,
)

# Keep LLVM's include paths and link flags, but drop its compile args:
# llvm-config injects its own -std=c++NN which overrides our c++23.
llvm_inc_link = llvm_dep.partial_dependency(
  includes: true,
  link_args: true,
  links: true,
  compile_args: false,
)

# Macros that llvm-config would normally have given us
llvm_args = [
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-D__STDC_LIMIT_MACROS',
]

# ---------------------------------------------------------------------------
# Optional Vulkan (toggle: -Dvulkan=enabled, default disabled)
# ---------------------------------------------------------------------------
vulkan_dep = dependency('vulkan', required: get_option('vulkan'))

# ---------------------------------------------------------------------------
# Builders (independent, only include error.hpp and base.h from include/)
# Ninja tracks header deps via depfiles, so they rebuild only when their
# source or those headers change.
# If a builder doesn't use the LLVM API (only prints IR text), remove the
# `dependencies` and `cpp_args` lines from it.
# ---------------------------------------------------------------------------
lexer_builder = executable('lexer_builder',
  'tools/lexer_builder.cpp',
  include_directories: inc,
  dependencies: llvm_inc_link,
  cpp_args: llvm_args,
  native: true,
)

parser_builder = executable('parser_builder',
  'tools/parser_builder.cpp',
  include_directories: inc,
  dependencies: llvm_inc_link,
  cpp_args: llvm_args,
  native: true,
)

# ---------------------------------------------------------------------------
# Generation: spec -> .ll + .h
#
# output order defines the [0]/[1] indexes AND @OUTPUT0@/@OUTPUT1@:
#   [0] / @OUTPUT0@ = .ll     [1] / @OUTPUT1@ = .h
# If your builders expect the header first on the command line, swap the
# @OUTPUT0@ / @OUTPUT1@ arguments in `command`, NOT the output list.
#
# The two targets don't depend on each other, so Ninja runs them in parallel.
# ---------------------------------------------------------------------------
lexer_gen = custom_target('lexer_gen',
  input: 'specs/lexer.def',
  output: ['lexer.h', 'lexer.ll'],
  command: [lexer_builder, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
  depend_files: files('include/base.h'),
)

parser_gen = custom_target('parser_gen',
  input: ['specs/lexer.def', 'specs/parser.def'],
  output: ['parser.h', 'parser.ll'],
  command: [parser_builder, '@INPUT0@', '@INPUT1@', '@OUTPUT0@', '@OUTPUT1@'],
  depend_files: files('include/base.h', 'include/error.hpp', 'src/error.cpp'),
)

# ---------------------------------------------------------------------------
# Dependency carrying include paths + generated headers.
# Attach it to every target that includes lexer.h / parser.h.
# ---------------------------------------------------------------------------
proj_dep = declare_dependency(
  include_directories: [inc, gen_inc],
  sources: [lexer_gen[1], parser_gen[1]],   # headers: guarantees they exist first
)

# ---------------------------------------------------------------------------
# Main program
# ---------------------------------------------------------------------------
app_sources = files(
  'src/main.cpp',
  'src/error.cpp',
  # ... list the rest of your src/*.cpp here (Meson has no globbing)
)

app_deps = [proj_dep, llvm_inc_link]
app_args = llvm_args

if vulkan_dep.found()
  app_deps += vulkan_dep
  app_args += '-DUSE_VULKAN=1'
endif

executable('app',
  app_sources,
  lexer_gen[0],     # .ll files: Meson compiles LLVM IR natively with clang
  parser_gen[0],
  dependencies: app_deps,
  cpp_args: app_args,
)
