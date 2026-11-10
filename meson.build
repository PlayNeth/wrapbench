project('wrapbench', 'cpp',
  version : '1.0.2',
  license : 'GPL-3.0-or-later',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

cpp = meson.get_compiler('cpp')
if host_machine.system() != 'windows' or host_machine.cpu_family() != 'x86'
  error('wrapbench targets 32-bit Windows (as the games do); use --cross-file build-win32.txt')
endif

deps = []
foreach lib : ['ddraw', 'gdi32', 'user32', 'version']
  deps += cpp.find_library(lib)
endforeach

executable('wrapbench', 'src/wrapbench.cpp',
  dependencies : deps,
  link_args : ['-static', '-static-libgcc', '-static-libstdc++', '-mwindows'])
