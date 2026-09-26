cmocka_dep = dependency('cmocka', required : false)
if cmocka_dep.found()
  foreach t : ['http_parser', 'router', 'balancer', 'config', 'core']
    exe = executable('test_' + t, 'unit/test_' + t + '.c',
      dependencies : [proxycore_dep, cmocka_dep])
    test(t, exe, suite : 'unit', timeout : 60)
  endforeach
else
  warning('cmocka no encontrado: tests unitarios desactivados')
endif

integration = find_program('integration/run.sh', required : false)
if integration.found()
  test('integration', integration,
    args : [meson.project_build_root()],
    suite : 'integration', timeout : 300, is_parallel : false)
endif
