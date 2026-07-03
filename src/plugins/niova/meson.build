# SPDX-FileCopyrightText: Copyright (c) 2026 NIOVA Systems, Inc.
# SPDX-License-Identifier: Apache-2.0

# NIOVA Plugin for NIXL
message('Building NIOVA plugin')

plugin_deps = [nixl_infra, nixl_common_dep]

# Define source files
niova_sources = [
    'niova_backend.cpp',
    'niova_backend.h',
    'niova_plugin.cpp',
]

compile_defs = []
plugin_link_args = []

if 'NIOVA' in static_plugins
    niova_backend_lib = static_library('NIOVA',
        niova_sources,
        dependencies: plugin_deps,
        link_args: plugin_link_args,
        cpp_args: compile_defs + compile_flags,
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: false,
        name_prefix: 'libplugin_')
else
    niova_backend_lib = shared_library('NIOVA',
        niova_sources,
        dependencies: plugin_deps,
        link_args: plugin_link_args,
        cpp_args: compile_defs + ['-fPIC'],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        install: true,
        name_prefix: 'libplugin_',
        install_dir: plugin_install_dir,
        install_rpath: '$ORIGIN/..')
    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
            'echo "NIOVA=' + niova_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
            check: true
        )
    endif
endif

niova_backend_interface = declare_dependency(link_with: niova_backend_lib)
