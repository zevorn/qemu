qtests_riscv64 = [
    'g233-cpu-test',
    'g233-dev-test'
]
qtests = {}
qtest_executables = {}
foreach dir : target_dirs
  if not dir.endswith('-softmmu')
    continue
  endif

  target_base = dir.split('-')[0]
  qtest_emulator = emulators['qemu-system-' + target_base]
  target_qtests = get_variable('qtests_' + target_base, [])
  has_kvm = ('CONFIG_KVM' in config_all_accel and host_os == 'linux'
             and cpu == target_base and fs.exists('/dev/kvm'))

  test_deps = roms
  qtest_env = environment()
  if have_tools
    qtest_env.set('QTEST_QEMU_IMG', './qemu-img')
    test_deps += [qemu_img]
  endif
  qtest_env.set('G_TEST_DBUS_DAEMON', meson.project_source_root() / 'tests/dbus-vmstate-daemon.sh')
  qtest_env.set('QTEST_QEMU_BINARY', './qemu-system-' + target_base)
  if have_tools and have_vhost_user_blk_server
    qtest_env.set('QTEST_QEMU_STORAGE_DAEMON_BINARY', './storage-daemon/qemu-storage-daemon')
    test_deps += [qsd]
  endif

  qtest_env.set('PYTHON', python.full_path())

  foreach test : target_qtests
    # Executables are shared across targets, declare them only the first time we
    # encounter them
    if not qtest_executables.has_key(test)
      src = [test + '.c']
      deps = [qemuutil, qos]
      if test in qtests
        # use a sourceset to quickly separate sources and deps
        test_ss = ss.source_set()
        test_ss.add(qtests[test])
        src += test_ss.all_sources()
        deps += test_ss.all_dependencies()
      endif
      qtest_executables += {
        test: executable(test, src, dependencies: deps)
      }
    endif

    test_args = ['--tap', '-k']

    if test == 'migration-test' and has_kvm
      test_args += ['--full']
    endif

    test('qtest-@0@/@1@'.format(target_base, test),
         qtest_executables[test],
         depends: [test_deps, qtest_emulator, emulator_modules],
         env: qtest_env,
         args: test_args,
         protocol: 'tap',
         timeout: 60,
         priority: 60,
         suite: ['qtest', 'qtest-' + target_base])
  endforeach
endforeach