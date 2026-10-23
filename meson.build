project('cressida', ['c', 'cpp'],
  version: '0.1.0',
  default_options: ['cpp_std=c++23', 'cpp_extensions=false', 'warning_level=1'],
)

# Rendering and Discord networking remain separate libraries.
render_deps = []
foreach name : [
  'wayland-client', 'wayland-egl', 'egl', 'glesv2',
  'cairo', 'cairo-ft', 'pango', 'pangocairo', 'pangoft2',
  'freetype2', 'fontconfig', 'harfbuzz', 'librsvg-2.0',
  'libwebp', 'libwebpdemux', 'libjxl', 'libjxl_threads',
  'xkbcommon', 'nlohmann_json', 'threads',
]
  render_deps += dependency(name)
endforeach
stb_dep = dependency('stb', required: false)
if stb_dep.found()
  render_deps += stb_dep
endif
cpp = meson.get_compiler('cpp')
if not cpp.has_header('stb/stb_image_resize2.h', dependencies: render_deps) and not cpp.has_header('stb_image_resize2.h', dependencies: render_deps)
  error('A recent stb with stb_image_resize2.h is required (see PACKAGING.md).')
endif

scanner = find_program('wayland-scanner')
protocol_sources = []
foreach name : ['cursor-shape-v1', 'text-input-unstable-v3']
  xml = 'protocols/' + name + '.xml'
  protocol_sources += custom_target(name + '-header',
    input: xml,
    output: name + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += custom_target(name + '-code',
    input: xml,
    output: name + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

wayland_protocols = dependency('wayland-protocols')
xdg_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_sources = [
  custom_target('xdg-shell-header', input: xdg_xml, output: 'xdg-shell-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-shell-code', input: xdg_xml, output: 'xdg-shell-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

decoration_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
xdg_sources += [
  custom_target('xdg-decoration-header', input: decoration_xml, output: 'xdg-decoration-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
  custom_target('xdg-decoration-code', input: decoration_xml, output: 'xdg-decoration-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
]

foreach protocol : [
  ['viewporter', 'stable/viewporter/viewporter.xml'],
  ['fractional-scale-v1', 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['xdg-activation-v1', 'staging/xdg-activation/xdg-activation-v1.xml'],
]
  xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / protocol[1]
  generated_protocol = [
    custom_target(protocol[0] + '-header', input: xml, output: protocol[0] + '-client-protocol.h',
      command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@']),
    custom_target(protocol[0] + '-code', input: xml, output: protocol[0] + '-protocol.c',
      command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@']),
  ]
  xdg_sources += generated_protocol
  if protocol[0] == 'xdg-activation-v1'
    activation_sources = generated_protocol
  endif
endforeach

subdir('src')
render_includes = include_directories('src', 'third_party/wuffs')
log_library = shared_library('cressida-log', 'src/core/log.cpp',
  include_directories: include_directories('src'), dependencies: dependency('threads'), install: true)
log_dep = declare_dependency(link_with: log_library,
  include_directories: include_directories('src'), dependencies: dependency('threads'))
render_deps += log_dep
render_lib = shared_library('cressida-render', render_sources + control_sources, protocol_sources,
  include_directories: render_includes,
  dependencies: render_deps,
  cpp_args: [
    '-DCRESSIDA_INSTALL_PREFIX="' + get_option('prefix') + '"',
    '-DCRESSIDA_INSTALL_DATADIR="' + get_option('datadir') + '"',
  ],
  install_rpath: '$ORIGIN',
  install: true,
)
render_dep = declare_dependency(
  link_with: render_lib,
  include_directories: render_includes,
  dependencies: render_deps,
)

media_deps = [dependency('libavformat'), dependency('libavcodec'), dependency('libavutil'), dependency('libswscale'), dependency('libwebpdemux'), dependency('libzstd'), dependency('rlottie'), dependency('nlohmann_json')]
media_library = static_library('cressida-media', 'src/media/animation.cpp', 'src/media/lottie.cpp',
  include_directories: include_directories('src'), dependencies: media_deps)
media_dep = declare_dependency(link_with: media_library, include_directories: include_directories('src'), dependencies: media_deps)
video_library = static_library('cressida-video', 'src/media/video_player.cpp',
  include_directories: include_directories('src'), dependencies: [dependency('mpv', version: '>=2.3'), dependency('threads'), log_dep])
video_dep = declare_dependency(link_with: video_library, include_directories: include_directories('src'), dependencies: [dependency('threads'), log_dep])
backend_deps = [dependency('libcurl', version: '>=8.16'), dependency('nlohmann_json'), dependency('threads')]
if meson.can_run_host_binaries()
  websocket_check = cpp.run('''
    #include <curl/curl.h>
    #include <cstring>
    int main() {
      for (const char* const* protocol = curl_version_info(CURLVERSION_NOW)->protocols; *protocol; ++protocol)
        if (std::strcmp(*protocol, "wss") == 0) return 0;
      return 1;
    }
  ''', dependencies: backend_deps, name: 'libcurl secure WebSockets')
  if not websocket_check.compiled() or websocket_check.returncode() != 0
    error('libcurl needs WebSocket support. Re-enter nix develop to use the project curl build, then reconfigure.')
  endif
endif
credential_deps = [dependency('libsecret-1'), dependency('threads'), log_dep]
credentials = static_library('credentials', 'src/auth/credentials.cpp', 'src/auth/secret_service.cpp',
  include_directories: include_directories('src'), dependencies: credential_deps)
credential_dep = declare_dependency(link_with: credentials,
  include_directories: include_directories('src'), dependencies: credential_deps)
backend = static_library('cressida-backend', 'src/api/models.cpp', 'src/api/embed_components.cpp', 'src/api/client.cpp', 'src/api/rest_restrictions.cpp',
  'src/api/guild_settings.cpp', 'src/api/server_preferences.cpp', 'src/api/settings.cpp', 'src/api/user_settings.cpp', 'src/api/gifs.cpp', 'src/api/emoji.cpp', 'src/api/stickers.cpp', 'src/api/read_state.cpp', 'src/api/role_styles.cpp', 'src/api/profile.cpp', 'src/api/cosmetics.cpp', 'src/api/threads.cpp', 'src/api/channel_features.cpp', 'src/api/message_search.cpp', 'src/api/inbox.cpp', 'src/api/user_directory.cpp', 'src/api/actions.cpp', 'src/api/assets.cpp', 'src/api/gateway.cpp', 'src/api/uploads.cpp', 'src/api/video_source.cpp',
  include_directories: include_directories('src'),
  dependencies: backend_deps + [credential_dep, media_dep, log_dep],
)
backend_dep = declare_dependency(link_with: backend,
  include_directories: include_directories('src'), dependencies: backend_deps + [credential_dep, media_dep, log_dep])

browser_opener = find_program('xdg-open', required: false)
browser_args = []
if browser_opener.found()
  browser_args += '-DCRESSIDA_OPEN_URI="' + browser_opener.full_path() + '"'
endif
browser_login = static_library('browser-login', 'src/auth/browser_login.cpp',
  include_directories: include_directories('src'),
  dependencies: [dependency('nlohmann_json'), dependency('threads'), log_dep],
  cpp_args: browser_args,
)
browser_dep = declare_dependency(link_with: browser_login,
  include_directories: include_directories('src'), dependencies: [dependency('threads'), log_dep])

client_args = []
# Keep local-prefix and staged installs runnable without LD_LIBRARY_PATH or ldconfig.
# Python's relpath also handles custom absolute bindir/libdir values on Meson 1.1.
python = import('python').find_installation('python3')
renderer_relative_dir = run_command(python, '-c',
  'import os, sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))',
  get_option('prefix') / get_option('libdir'), get_option('prefix') / get_option('bindir'),
  check: true,
).stdout().strip()
if get_option('emoji_font') != ''
  client_args += '-DCRESSIDA_EMOJI_FONT="' + get_option('emoji_font') + '"'
endif
markdown_dep = dependency('md4c')
message_layout = static_library('message-layout', ['src/app/message_layout.cpp', 'src/app/syntax_highlight.cpp'], dependencies: [render_dep, backend_dep, markdown_dep])
message_layout_dep = declare_dependency(link_with: message_layout, dependencies: [render_dep, backend_dep, markdown_dep])
emoji_catalog = static_library('emoji-catalog', 'src/app/emoji_catalog.cpp', dependencies: [backend_dep, markdown_dep])
theme_deps = [dependency('tomlplusplus'), dependency('gio-2.0'), dependency('threads'), log_dep]
theme_library = static_library('cressida-themes', 'src/app/theme_catalog.cpp', 'src/app/theme_runtime.cpp',
  include_directories: include_directories('src'), dependencies: theme_deps)
theme_dep = declare_dependency(link_with: theme_library, include_directories: include_directories('src'), dependencies: theme_deps)
client = executable('cressida',
  'src/main.cpp', 'src/app/single_instance.cpp', 'src/app/chat_navigation.cpp', 'src/app/window.cpp', 'src/app/window_activation.cpp', 'src/app/window_icons.cpp', 'src/app/window_control_view.cpp', 'src/app/window_controls_demo.cpp', 'src/app/font_catalog.cpp', 'src/app/autocomplete.cpp', 'src/app/chat_preload.cpp', 'src/app/media_view.cpp', 'src/app/sidebar_view.cpp', 'src/app/server_rail.cpp', 'src/app/server_rail_demo.cpp', 'src/app/server_rail_drag.cpp', 'src/app/dm_home.cpp', 'src/app/dm_profile.cpp', 'src/app/dm_profile_demo.cpp', 'src/app/dm_create.cpp', 'src/app/dm_request_panel.cpp', 'src/app/dm_home_demo.cpp', 'src/app/video_view.cpp', 'src/app/role_view.cpp', 'src/app/system_message_view.cpp', 'src/app/message_log_view.cpp', 'src/app/message_dialog.cpp', 'src/app/profile_view.cpp', 'src/app/profile_cosmetics.cpp', 'src/app/profile_cosmetics_demo.cpp', 'src/app/name_badges.cpp', 'src/app/guild_profile_view.cpp', 'src/app/server_view.cpp', 'src/app/thread_view.cpp', 'src/app/forum_view.cpp', 'src/app/channel_view.cpp', 'src/app/component_view.cpp', 'src/app/channel_demo.cpp', 'src/app/channel_menu.cpp', 'src/app/channel_create.cpp', 'src/app/channel_create_demo.cpp', 'src/app/channel_invite.cpp', 'src/app/settings_color_picker.cpp', 'src/app/server_commands_demo.cpp', 'src/app/server_commands.cpp', 'src/app/server_commands_model.cpp', 'src/app/server_management.cpp', 'src/app/server_audio.cpp', 'src/app/server_audit.cpp', 'src/app/server_audit_model.cpp', 'src/app/server_expressions.cpp', 'src/app/server_image_crop.cpp', 'src/app/server_extra_forms.cpp', 'src/app/server_settings.cpp', 'src/app/server_settings_demo.cpp', 'src/app/server_menu.cpp', 'src/app/server_menu_dialog.cpp', 'src/app/server_menu_demo.cpp', 'src/app/typing_view.cpp', 'src/app/server_upload.cpp', 'src/app/file_picker.cpp', 'src/app/select_popup.cpp', 'src/app/gif_picker.cpp', 'src/app/emoji_picker.cpp', 'src/app/sticker_picker.cpp', 'src/app/user_settings.cpp', 'src/app/user_preferences.cpp', 'src/app/user_preferences_demo.cpp', 'src/app/desktop_notifications.cpp', 'src/app/desktop_tray.cpp', 'src/app/tray_view.cpp', 'src/app/user_notifications.cpp', 'src/app/user_account_view.cpp', 'src/app/user_account_demo.cpp', 'src/app/user_account_dialog.cpp', 'src/app/user_account_profile.cpp', 'src/app/user_settings_lists.cpp', 'src/app/native_controls.cpp', 'src/app/context_actions.cpp', 'src/app/client_application.cpp', 'src/app/image_cache.cpp', 'src/app/outgoing_messages.cpp', 'src/app/search_view.cpp', 'src/app/inbox_view.cpp', 'src/app/connection_state.cpp', 'src/app/session_store.cpp', xdg_sources, protocol_sources[0], protocol_sources[1],
  dependencies: [render_dep, backend_dep, browser_dep, markdown_dep, video_dep, theme_dep],
  link_with: [message_layout, emoji_catalog],
  cpp_args: client_args,
  install_rpath: '$ORIGIN' / renderer_relative_dir,
  install: true,
)

install_subdir('src',
  install_dir: get_option('includedir') / 'cressida',
  strip_directory: true,
  exclude_files: ['core/log.cpp', 'app/single_instance.cpp', 'app/autocomplete.cpp', 'app/syntax_highlight.cpp', 'ui/scroll_into_view.cpp', 'app/typing_view.cpp', 'app/chat_navigation.cpp', 'api/embed_components.cpp', 'api/server_preferences.cpp', 'app/server_menu_dialog.cpp', 'app/server_menu_demo.cpp', 'app/chat_preload.cpp', 'app/dm_home.cpp', 'app/dm_profile.cpp', 'app/dm_profile_demo.cpp', 'app/dm_create.cpp', 'app/dm_request_panel.cpp', 'app/dm_home_demo.cpp', 'app/user_preferences_demo.cpp', 'app/user_account_demo.cpp', 'app/desktop_notifications.cpp', 'app/desktop_tray.cpp', 'app/tray_view.cpp', 'app/user_notifications.cpp', 'api/user_settings.cpp', 'app/user_preferences.cpp', 'app/user_account_view.cpp', 'app/user_account_dialog.cpp', 'app/user_account_profile.cpp', 'app/user_settings_lists.cpp', 'app/connection_state.cpp', 'app/server_commands_demo.cpp', 'app/server_commands.cpp', 'app/server_commands_model.cpp', 'app/server_image_crop.cpp', 'media/lottie.cpp', 'app/server_extra_forms.cpp', 'app/server_expressions.cpp', 'app/server_audit_model.cpp', 'app/server_audit.cpp', 'app/server_audio.cpp', 'app/server_management.cpp', 'app/settings_color_picker.cpp', 'api/guild_settings.cpp', 'app/server_settings.cpp', 'app/server_settings_demo.cpp', 'app/server_menu.cpp', 'app/server_upload.cpp', 'app/file_picker.cpp', 'app/select_popup.cpp', 'app/channel_demo.cpp', 'app/channel_menu.cpp', 'app/channel_create.cpp', 'app/channel_create_demo.cpp', 'app/channel_invite.cpp', 'app/channel_view.cpp', 'app/component_view.cpp', 'api/channel_features.cpp', 'api/message_search.cpp', 'api/inbox.cpp', 'api/threads.cpp', 'app/thread_view.cpp', 'app/forum_view.cpp'] + render_source_paths + control_source_paths + ['app/sticker_picker.cpp', 'api/stickers.cpp', 'app/emoji_picker.cpp', 'app/emoji_catalog.cpp', 'api/emoji.cpp', 'api/role_styles.cpp', 'api/profile.cpp', 'api/cosmetics.cpp', 'app/profile_view.cpp', 'app/profile_cosmetics.cpp', 'app/profile_cosmetics_demo.cpp', 'app/name_badges.cpp', 'app/guild_profile_view.cpp', 'app/server_view.cpp', 'app/role_view.cpp', 'app/system_message_view.cpp', 'app/message_log_view.cpp', 'app/message_dialog.cpp', 'app/video_view.cpp', 'media/video_player.cpp', 'api/video_source.cpp'] + ['meson.build', 'main.cpp', 'app/theme_catalog.cpp', 'app/theme_runtime.cpp', 'app/font_catalog.cpp', 'app/window.cpp', 'app/window_activation.cpp', 'app/window_icons.cpp', 'app/window_control_view.cpp', 'app/window_controls_demo.cpp', 'app/media_view.cpp', 'app/sidebar_view.cpp', 'app/server_rail.cpp', 'app/server_rail_demo.cpp', 'app/server_rail_drag.cpp', 'app/gif_picker.cpp', 'app/user_settings.cpp', 'app/native_controls.cpp', 'app/context_actions.cpp', 'app/client_application.cpp', 'app/image_cache.cpp', 'app/outgoing_messages.cpp', 'app/search_view.cpp', 'app/inbox_view.cpp', 'app/session_store.cpp', 'app/message_layout.cpp', 'api/client.cpp', 'api/rest_restrictions.cpp', 'api/models.cpp', 'api/settings.cpp', 'api/gifs.cpp', 'api/read_state.cpp', 'api/user_directory.cpp', 'api/actions.cpp', 'api/assets.cpp', 'api/gateway.cpp', 'api/uploads.cpp', 'auth/browser_login.cpp', 'auth/credentials.cpp', 'auth/secret_service.cpp', 'media/animation.cpp'],
)
install_subdir('assets',
  install_dir: get_option('datadir') / 'cressida',
)
install_data('LICENSE', 'third_party/wuffs/LICENSE-APACHE', 'third_party/wuffs/LICENSE-MIT',
  install_dir: get_option('datadir') / 'licenses/cressida',
)

log_test = executable('log_test', 'tests/log_test.cpp', dependencies: log_dep)
test('log', log_test)
request_diagnostics_test = executable('request_diagnostics_test', 'tests/request_diagnostics_test.cpp', dependencies: log_dep)
test('request_diagnostics', request_diagnostics_test)

# Reuse upstream regression coverage for the copied text and image paths.
foreach name : ['cairo_text_renderer', 'image_orientation', 'image_file_loader_data_uri']
  check = executable(name + '_test',
    'tests/' + name + '_test.cpp',
    dependencies: render_dep,
  )
  test(name, check,
    env: ['CRESSIDA_ASSETS_DIR=' + meson.project_source_root() / 'assets'],
  )
endforeach

glyph_gradient_test = executable('glyph_gradient_test', 'tests/glyph_gradient_test.cpp', dependencies: render_dep)
test('glyph_gradient', glyph_gradient_test)
text_sharpness_test = executable('text_sharpness_test', 'tests/text_sharpness_test.cpp', dependencies: render_dep)
test('text_sharpness', text_sharpness_test)
surface_scale_test = executable('surface_scale_test', 'tests/surface_scale_test.cpp', include_directories: include_directories('src'))
test('surface_scale', surface_scale_test)
folder_motion_test = executable('folder_motion_test', 'tests/folder_motion_test.cpp', include_directories: include_directories('src'))
test('folder_motion', folder_motion_test)

model_test = executable('discord_models_test', 'tests/discord_models_test.cpp', dependencies: backend_dep)
test('discord_models', model_test)
client_test = executable('discord_client_test', 'tests/discord_client_test.cpp', dependencies: backend_dep)
test('discord_client', client_test)
test('client_help', client, args: ['--help'])
test('client_version', client, args: ['--version'])
test('client_log_level_help', client, args: ['--log-level', 'debug', '--help'])
test('client_log_level_invalid', client, args: ['--log-level', 'invalid'], should_fail: true)
test('client_log_level_missing', client, args: ['--log-level'], should_fail: true)
test('client_start_minimized_help', client, args: ['--start-minimized', '--help'])
test('client_start_minimized_version', client, args: ['--start-minimized', '--start-minimized', '--version'])
test('client_start_minimized_invalid', client, args: ['--start-minimized=true'], should_fail: true)
test('installation', python,
  args: [files('tests/installation_test.py'), meson.project_build_root(), meson.project_source_root(),
    find_program('meson').full_path(), find_program('readelf').full_path()],
  depends: client, timeout: 30,
)
browser_test = executable('browser_login_test', 'tests/browser_login_test.cpp', dependencies: browser_dep)
test('browser_login', browser_test)
state_test = executable('discord_state_test', 'tests/discord_state_test.cpp', dependencies: backend_dep)
test('discord_state', state_test)
layout_test = executable('message_layout_test', 'tests/message_layout_test.cpp', dependencies: message_layout_dep, link_with: emoji_catalog)
test('message_layout', layout_test, args: [meson.project_source_root() / 'assets'])
embed_components_test = executable('embed_components_test', 'tests/embed_components_test.cpp', dependencies: message_layout_dep)
test('embed_components', embed_components_test)
sticker_test = executable('stickers_test', 'tests/stickers_test.cpp', dependencies: message_layout_dep)
test('stickers', sticker_test)
server_preferences_test = executable('server_preferences_test', 'tests/server_preferences_test.cpp', dependencies: backend_dep)
test('server_preferences', server_preferences_test)
server_organization_test = executable('server_organization_test', 'tests/server_organization_test.cpp', dependencies: backend_dep)
test('server_organization', server_organization_test)
completion_test = executable('autocomplete_test', 'tests/autocomplete_test.cpp', include_directories: include_directories('src'))
test('autocomplete', completion_test)
chat_links_test = executable('chat_links_test', 'tests/chat_links_test.cpp', dependencies: message_layout_dep)
test('chat_links', chat_links_test)
gateway_test = executable('gateway_test', 'tests/gateway_test.cpp', dependencies: backend_dep)
test('gateway', find_program('python3'), args: [files('tests/gateway_fixture.py'), gateway_test], timeout: 15)
session_test = executable('session_store_test', 'tests/session_store_test.cpp', 'src/app/session_store.cpp', dependencies: backend_dep)
test('session_store', session_test)
font_catalog_test = executable('font_catalog_test', 'tests/font_catalog_test.cpp', 'src/app/font_catalog.cpp',
  include_directories: include_directories('src'), dependencies: dependency('fontconfig'))
test('font_catalog', font_catalog_test)
theme_test = executable('theme_catalog_test', 'tests/theme_catalog_test.cpp', dependencies: theme_dep)
test('theme_catalog', theme_test, args: [meson.current_source_dir() / 'assets/themes'])
theme_runtime_test = executable('theme_runtime_test', 'tests/theme_runtime_test.cpp', dependencies: theme_dep)
test('theme_runtime', theme_runtime_test, args: [meson.current_source_dir() / 'assets/themes'], timeout: 20)

install_data('assets/dev.lysec.Cressida.desktop', install_dir: get_option('datadir') / 'applications')
install_data('assets/cressida-logo.svg', rename: 'dev.lysec.Cressida.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
# These tools are optional; importing this module adds no GTK/GNOME UI dependency.
# Meson skips the cache refreshes for DESTDIR/package staging.
import('gnome').post_install(
  gtk_update_icon_cache: find_program('gtk-update-icon-cache', required: false).found(),
  update_desktop_database: find_program('update-desktop-database', required: false).found(),
)
editor_test = executable('text_edit_test', 'tests/text_edit_test.cpp', dependencies: render_dep)
test('text_edit', editor_test)

uploads_test = executable('uploads_test', 'tests/uploads_test.cpp', dependencies: backend_dep)
test('uploads', find_program('python3'), args: [files('tests/uploads_fixture.py'), uploads_test], timeout: 12)

presence_test = executable('presence_test', 'tests/presence_test.cpp', dependencies: backend_dep)
test('presence', presence_test)

read_state_test = executable('read_state_test', 'tests/read_state_test.cpp', dependencies: backend_dep)
test('read_state', read_state_test)
role_style_test = executable('role_style_test', 'tests/role_style_test.cpp', dependencies: backend_dep)
test('role_style', role_style_test)
profile_test = executable('profile_test', 'tests/profile_test.cpp', dependencies: backend_dep)
test('profile', profile_test)
cosmetics_test = executable('cosmetics_test', 'tests/cosmetics_test.cpp', dependencies: backend_dep)
test('cosmetics', cosmetics_test)
thread_test = executable('threads_test', 'tests/threads_test.cpp', dependencies: backend_dep)
test('threads', thread_test)
channel_features_test = executable('channel_features_test', 'tests/channel_features_test.cpp', dependencies: backend_dep)
test('channel_features', channel_features_test)
message_search_test = executable('message_search_test', 'tests/message_search_test.cpp', dependencies: backend_dep)
test('message_search', message_search_test)
inbox_test = executable('inbox_test', 'tests/inbox_test.cpp', dependencies: backend_dep)
test('inbox', inbox_test)
control_test = executable('native_controls_test', 'tests/native_controls_test.cpp', dependencies: render_dep)
test('native_controls', control_test)

directory_test = executable('user_directory_test', 'tests/user_directory_test.cpp', dependencies: backend_dep)
test('user_directory', directory_test)

interactions_test = executable('interactions_test', 'tests/interactions_test.cpp', dependencies: backend_dep)
test('interactions', interactions_test)

context_menu_test = executable('context_menu_test', 'tests/context_menu_test.cpp', dependencies: render_dep)
test('context_menu', context_menu_test)

credentials_test = executable('credentials_test', 'tests/credentials_test.cpp', dependencies: credential_dep)
test('credentials', credentials_test)
secret_service_test = executable('secret_service_test', 'tests/secret_service_test.cpp', dependencies: credential_dep)
test('secret_service', secret_service_test,
  env: ['DBUS_SESSION_BUS_ADDRESS=unix:path=/cressida-test-no-session-bus'], timeout: 10)
credential_client_test = executable('credential_client_test', 'tests/credential_client_test.cpp', dependencies: backend_dep)
test('credential_client', credential_client_test)
rest_schedule_test = executable('rest_schedule_test', 'tests/rest_schedule_test.cpp',
  include_directories: include_directories('src'), dependencies: dependency('threads'))
test('rest_schedule', rest_schedule_test)
rest_rate_limits_test = executable('rest_rate_limits_test', 'tests/rest_rate_limits_test.cpp',
  include_directories: include_directories('src'), dependencies: dependency('threads'))
test('rest_rate_limits', rest_rate_limits_test)
rest_restrictions_test = executable('rest_restrictions_test', 'tests/rest_restrictions_test.cpp', dependencies: backend_dep)
test('rest_restrictions', rest_restrictions_test)
gateway_pacing_test = executable('gateway_pacing_test', 'tests/gateway_pacing_test.cpp',
  include_directories: include_directories('src'))
test('gateway_pacing', gateway_pacing_test)
read_acknowledgements_test = executable('read_acknowledgements_test', 'tests/read_acknowledgements_test.cpp', dependencies: backend_dep)
test('read_acknowledgements', read_acknowledgements_test)
rest_transfer_test = executable('rest_transfer_test', 'tests/rest_transfer_test.cpp',
  include_directories: include_directories('src'), dependencies: backend_deps)
test('rest_transfer', rest_transfer_test)

assets_test = executable('assets_test', 'tests/assets_test.cpp', dependencies: backend_dep)
test('assets', assets_test)

gifs_test = executable('gifs_test', 'tests/gifs_test.cpp', dependencies: backend_dep)
test('gifs', gifs_test)
animation_test = executable('animation_test', 'tests/animation_test.cpp', dependencies: media_dep)
test('animation', find_program('python3'), args: [files('tests/animation_fixture.py'), find_program('ffmpeg'), animation_test], timeout: 60)
image_cache_test = executable('image_cache_test', 'tests/image_cache_test.cpp', 'src/app/image_cache.cpp', dependencies: [media_dep, render_dep])
test('image_cache', image_cache_test)
animation_texture_test = executable('animation_texture_test', 'tests/animation_texture_test.cpp', dependencies: [media_dep, render_dep])
test('animation_texture', animation_texture_test)
scroll_test = executable('scroll_motion_test', 'tests/scroll_motion_test.cpp', dependencies: render_dep)
test('scroll_motion', scroll_test)
emoji_test = executable('emoji_catalog_test', 'tests/emoji_catalog_test.cpp', link_with: emoji_catalog, dependencies: [backend_dep, markdown_dep])
test('emoji_catalog', emoji_test, args: [meson.project_source_root() / 'assets'])
unread_timeline_test = executable('unread_timeline_test', 'tests/unread_timeline_test.cpp', dependencies: backend_dep)
test('unread_timeline', unread_timeline_test)
message_history_test = executable('message_history_test', 'tests/message_history_test.cpp', dependencies: backend_dep)
test('message_history', message_history_test)
message_group_test = executable('message_group_test', 'tests/message_group_test.cpp', dependencies: backend_dep)
test('message_group', message_group_test)
message_log_test = executable('message_log_test', 'tests/message_log_test.cpp', dependencies: backend_dep)
test('message_log', message_log_test)
system_message_test = executable('system_message_test', 'tests/system_message_test.cpp', dependencies: backend_dep)
test('system_message', system_message_test)
message_mentions_test = executable('message_mentions_test', 'tests/message_mentions_test.cpp', dependencies: backend_dep)
test('message_mentions', message_mentions_test)
video_test = executable('video_player_test', 'tests/video_player_test.cpp', dependencies: [video_dep, dependency('egl'), dependency('glesv2')])
test('video_player', find_program('python3'), args: [files('tests/video_fixture.py'), find_program('ffmpeg'), video_test], timeout: 40)

guild_settings_test = executable('guild_settings_test', 'tests/guild_settings_test.cpp', dependencies: backend_dep)
test('guild_settings', guild_settings_test)
file_picker_test = executable('file_picker_test', 'tests/file_picker_test.cpp', 'src/app/file_picker.cpp',
  include_directories: include_directories('src'), dependencies: [render_dep, dependency('gio-2.0'), dependency('threads')])
test('file_picker', file_picker_test, timeout: 20)

server_audit_test = executable('server_audit_test', 'tests/server_audit_test.cpp', 'src/app/server_audit_model.cpp', include_directories: include_directories('src'), dependencies: dependency('nlohmann_json'))
test('server_audit', server_audit_test)
audio_preview_test = executable('audio_preview_test', 'tests/audio_preview_test.cpp', dependencies: video_dep)
test('audio_preview', find_program('python3'), args: [files('tests/audio_fixture.py'), find_program('ffmpeg'), audio_preview_test], timeout: 30)
lottie_test = executable('lottie_test', 'tests/lottie_test.cpp', dependencies: media_dep)
test('lottie', lottie_test)

server_commands_test = executable('server_commands_test', 'tests/server_commands_test.cpp', 'src/app/server_commands_model.cpp', include_directories: include_directories('src'), dependencies: dependency('nlohmann_json'))
test('server_commands', server_commands_test)
server_recurrence_test = executable('server_recurrence_test', 'tests/server_recurrence_test.cpp', include_directories: include_directories('src'))
test('server_recurrence', server_recurrence_test)

user_settings_test = executable('user_settings_test', 'tests/user_settings_test.cpp', dependencies: backend_dep)
test('user_settings', user_settings_test)

direct_messages_test = executable('direct_messages_test', 'tests/direct_messages_test.cpp', dependencies: backend_dep)
test('direct_messages', direct_messages_test)

desktop_notifications_test = executable('desktop_notifications_test',
  'tests/desktop_notifications_test.cpp', 'src/app/desktop_notifications.cpp',
  include_directories: include_directories('src'), dependencies: [render_dep, dependency('gio-2.0'), dependency('threads')])
test('desktop_notifications', desktop_notifications_test, timeout: 15)
desktop_tray_test = executable('desktop_tray_test',
  'tests/desktop_tray_test.cpp', 'src/app/desktop_tray.cpp',
  include_directories: include_directories('src'), dependencies: [render_dep, dependency('gio-2.0'), dependency('threads')])
test('desktop_tray', desktop_tray_test, args: [meson.current_source_dir() / 'assets/cressida-tray.svg'], timeout: 20)
restart_process_test = executable('restart_process_test', 'tests/restart_process_test.cpp', include_directories: include_directories('src'))
test('restart_process', restart_process_test)

billing_amount_test = executable('billing_amount_test', 'tests/billing_amount_test.cpp',
  include_directories: include_directories('src'))
test('billing_amount', billing_amount_test)

typing_test = executable('typing_test', 'tests/typing_test.cpp', dependencies: backend_dep)
test('typing', typing_test)

window_decorations_test = executable('window_decorations_test', 'tests/window_decorations_test.cpp',
  include_directories: include_directories('src'))
test('window_decorations', window_decorations_test)

window_controls_test = executable('window_controls_test', 'tests/window_controls_test.cpp',
  include_directories: include_directories('src'))
test('window_controls', window_controls_test)
window_icons_test = executable('window_icons_test', 'tests/window_icons_test.cpp',
  'src/app/window_icons.cpp', 'src/app/file_picker.cpp',
  include_directories: include_directories('src'), dependencies: [render_dep, dependency('gio-2.0'), dependency('threads')])
test('window_icons', window_icons_test, timeout: 10)

settings_layout_test = executable('settings_layout_test', 'tests/settings_layout_test.cpp',
  include_directories: include_directories('src'))
test('settings_layout', settings_layout_test)

desktop_activation_test = executable('desktop_activation_test', 'tests/desktop_activation_test.cpp',
  include_directories: include_directories('src'))
test('desktop_activation', desktop_activation_test)

activation_server_header = custom_target('xdg-activation-server-header',
  input: wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'staging/xdg-activation/xdg-activation-v1.xml',
  output: 'xdg-activation-v1-server-protocol.h', command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
window_activation_test = executable('window_activation_test', 'tests/window_activation_test.cpp',
  'src/app/window_activation.cpp', activation_sources, activation_server_header,
  include_directories: include_directories('src'), dependencies: [render_dep, dependency('wayland-server'), dependency('threads')])
test('window_activation', window_activation_test, timeout: 10)

single_instance_test = executable('single_instance_test', 'tests/single_instance_test.cpp', 'src/app/single_instance.cpp',
  include_directories: include_directories('src'), dependencies: dependency('threads'))
test('single_instance', single_instance_test, args: [client], timeout: 30)
