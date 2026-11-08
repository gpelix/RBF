#===- meson.build ------------------------------------------*- Meson -*-===
#
# meson.build for rbfc
#
#===--------------------------------------------------------------------===

project(
  'rbfc',
  'cpp',
  'c',
  #default_options: ['cpp_std=c++20', 'buildtype=plain'],    # DEBUG
  default_options: ['cpp_std=c++20', 'buildtype=plain', 'optimization=3'], # RELEASE
  version: '0.1'
)

cpp = meson.get_compiler('cpp')
cc  = meson.get_compiler('c')

default_data_length = '30000'

add_global_arguments(['-D_VERSION=' + meson.project_version(),
                      '-D_CC="' + cc.get_id() + '"',   # used as linker by bfc
                      '-D_GNU_SOURCE',
                      '-D_DEFAULT_DATA_LENGTH=' + default_data_length + 'L'], language: 'cpp')

warning('If something goes wrong change march to native')

#add_global_arguments(['-g'], language: 'cpp') # DEBUG
add_global_arguments(['-DNDEBUG', '-march=native', '-fno-exceptions'], language: 'cpp') # RELEASE


if not cpp.has_header('getopt.h')
  error('Missing getopt.h')
endif

if not cpp.has_header('unistd.h')
  error('Only unix like OSs are supported')
endif

if not cpp.has_header('sys/wait.h')
  error('Are you on unix? Only unix like OSs are supported')
endif

code = '''
[[gnu::hot]]
void func() { }
'''

if cpp.compiles(code, name: 'gnu::hot')
  add_global_arguments(['-D_HAS_GNU_HOT'], language: 'cpp')
endif

llvm = dependency('llvm')

executable('rbfc', 'main.cc', dependencies: llvm)#, cpp_pch: 'pch/pch.h')
