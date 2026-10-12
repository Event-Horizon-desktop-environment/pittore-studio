# The AF import is a long chain of pixel loops: unoptimised it takes ~33s on
# a 115-layer project where -O2 takes 4.5s. -g stays on, so nothing is lost
# for debugging, and nothing in src/ relies on assert().
project('pittore-studio', 'cpp',
  version : '0.1.1',
  license : 'MIT',
  meson_version : '>=1.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
    'prefix=/usr',
  ])

if host_machine.system() != 'linux'
  error('Pittore Studio is Linux-only. Refusing to build elsewhere.')
endif

# GCC 16 probes incomplete Qt types in SFINAE contexts and notes it on every
# TU including Qt headers. Third-party header noise, not project code.
cxx = meson.get_compiler('cpp')
if cxx.has_argument('-Wno-sfinae-incomplete')
  add_project_arguments('-Wno-sfinae-incomplete', language : 'cpp')
endif

subdir('src')
subdir('tests')
subdir('tools')