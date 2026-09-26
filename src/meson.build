core_src = files(
  'util.c',
  'io_event_common.c',
  'buffer_pool.c',
  'log.c',
  'config.c',
  'router.c',
  'backend_pool.c',
  'http_parser.c',
  'tls.c',
  'health.c',
  'stats.c',
)

if system == 'linux'
  core_src += files('io_event_epoll.c', 'watch_inotify.c')
else
  core_src += files('io_event_kqueue.c', 'watch_kqueue.c')
endif

core_deps = [threads_dep, ssl_dep, toml_dep, m_dep]
core_inc = include_directories('.')

proxycore = static_library('proxycore', core_src,
  include_directories : core_inc,
  dependencies : core_deps)

proxycore_dep = declare_dependency(
  link_with : proxycore,
  include_directories : core_inc,
  dependencies : core_deps)

executable('proxy',
  'main.c', 'master.c', 'worker.c', 'runtime.c', 'connection.c', 'listener.c',
  dependencies : proxycore_dep,
  install : true)
