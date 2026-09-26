# tomlc99 (MIT, https://github.com/cktan/tomlc99) vendorizado
# commit 29076dfd095bbbbd50a3c1b2760d29f4b83e74ac
project('tomlc99', 'c', license : 'MIT', default_options : ['c_std=c99', 'warning_level=0'])
toml_lib = static_library('toml', 'toml.c', pic : true)
toml_dep = declare_dependency(link_with : toml_lib, include_directories : include_directories('.'))
