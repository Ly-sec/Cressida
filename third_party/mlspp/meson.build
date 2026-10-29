# Private C++23 integration for the attributed MLS++ snapshot.
mls_namespace = configuration_data()
mls_namespace.set('MLS_CXX_NAMESPACE', 'cressida_mls')
configure_file(input: 'cmake/namespace.h.in', output: 'namespace.h', configuration: mls_namespace)
mls_crypto = dependency('libcrypto', version: '>=3.0')
mls_json = dependency('nlohmann_json')
mls_includes = include_directories('.', 'include', 'lib/bytes/include', 'lib/tls_syntax/include', 'lib/hpke/include')
mls_sources = files(
  'lib/bytes/src/bytes.cpp',
  'lib/hpke/src/aead_cipher.cpp',
  'lib/hpke/src/base64.cpp',
  'lib/hpke/src/certificate.cpp',
  'lib/hpke/src/common.cpp',
  'lib/hpke/src/dhkem.cpp',
  'lib/hpke/src/digest.cpp',
  'lib/hpke/src/group.cpp',
  'lib/hpke/src/hkdf.cpp',
  'lib/hpke/src/hpke.cpp',
  'lib/hpke/src/openssl_common.cpp',
  'lib/hpke/src/random.cpp',
  'lib/hpke/src/rsa.cpp',
  'lib/hpke/src/signature.cpp',
  'lib/hpke/src/userinfo_vc.cpp',
  'lib/tls_syntax/src/tls_syntax.cpp',
  'src/common.cpp',
  'src/core_types.cpp',
  'src/credential.cpp',
  'src/crypto.cpp',
  'src/grease.cpp',
  'src/key_schedule.cpp',
  'src/messages.cpp',
  'src/session.cpp',
  'src/state.cpp',
  'src/tree_math.cpp',
  'src/treekem.cpp',
 )
mls_library = static_library('cressida-mls', mls_sources,
  include_directories: mls_includes,
  dependencies: [mls_crypto, mls_json],
  cpp_args: ['-DWITH_OPENSSL3', '-DDISABLE_GREASE'],
  gnu_symbol_visibility: 'hidden',
  install: false,
)
mls_dep = declare_dependency(link_with: mls_library, include_directories: mls_includes,
  dependencies: [mls_crypto, mls_json], compile_args: ['-DWITH_OPENSSL3', '-DDISABLE_GREASE'])
