project('proxy', 'c',
  version : '1.0.0',
  license : 'MIT',
  meson_version : '>= 0.60',
  default_options : ['c_std=c11', 'warning_level=3', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
system = host_machine.system()

if system == 'linux'
  add_project_arguments('-D_GNU_SOURCE', language : 'c')
elif system == 'darwin'
  add_project_arguments('-D_DARWIN_C_SOURCE', language : 'c')
else
  # FreeBSD / OpenBSD / NetBSD: kqueue
  add_project_arguments('-D_BSD_SOURCE', '-D__BSD_VISIBLE=1', language : 'c')
endif

threads_dep = dependency('threads')
ssl_dep = dependency('openssl', version : '>= 3.0')
toml_dep = dependency('tomlc99', fallback : ['tomlc99', 'toml_dep'], required : true)
m_dep = cc.find_library('m', required : false)

subdir('src')
subdir('tools')
subdir('tests')
