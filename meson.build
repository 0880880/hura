project('hura', 'c', 'cpp',
  version : '0.1',
  default_options : [
    'warning_level=2',
    'c_std=c11',
    'cpp_std=c++23',
    'buildtype=debug'
  ])

# ====================== VULKAN ======================
# Reserved for the future SPIR-V / multithreaded backend; not used yet.
vulkan_dep = dependency('vulkan', required : false)

llvm_modules = ['core']

llvm_dep = dependency(
  'llvm',
  version : '>=21.0',
  modules : llvm_modules,
  static : false,
)

# tools/ provides lp_dep (generated lexer/parser objects + headers)
subdir('tools')

# src/ provides the hura static library
subdir('src')
