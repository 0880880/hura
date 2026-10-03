project('hura', 'c', 'cpp',
  version : '0.1',
  default_options : [
    'warning_level=2',
    'c_std=c11',
    'cpp_std=c++23',
    'buildtype=debug'
  ])

# ====================== VULKAN ======================
vulkan_dep = dependency('vulkan', required : true)

llvm_modules = ['core']

llvm_dep = dependency(
  'llvm',
  version : '>=21.0', # Optional version requirement
  modules : llvm_modules,
  static : false # Set to true if you want static linking
)

executable('hura',
  sources : ['src/main.cpp'],
  dependencies : [llvm_dep],
  install : true)
