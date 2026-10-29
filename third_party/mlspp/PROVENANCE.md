# Bundled MLS++

Upstream: https://github.com/cisco/mlspp

Revision: `1cc50a124a3bc4e143a787ec934280dc70c1034d`.
Archive SHA-256: `7a9d6318627e548903bc65c3dc5a4de4a90290983efea9a49af8561ed3f999f5`.

The source snapshot retains the root LICENSE, include/, src/, lib/bytes/include
and src, lib/tls_syntax/include and src, lib/hpke/include and src, and
cmake/namespace.h.in. upstream-files.json records each original file's SHA-256.
The BSD-2-Clause license and Cisco Systems' copyright remain in LICENSE.

The unused vcpkg tooling, delivery service, vector generator, test tooling and
legacy variant compatibility header are omitted. C++23 uses std::variant.
No separately licensed variant header is redistributed.

Cressida supplies meson.build to compile one private static archive with C++23,
system OpenSSL 3, system JSON, namespace cressida_mls and DISABLE_GREASE.
The archive and its headers are not installed as a public MLS library.

Local patches:

- lib/tls_syntax/src/tls_syntax.cpp checks an empty input before reading a
  variable-length integer prefix.
- lib/tls_syntax/include/tls/tls_syntax.h rejects trailing bytes in single-object
  unmarshal. Explicit multi-object messages use a stream instead.
- The vector reader in the same header rejects elements that consume no input.
- lib/bytes/include/bytes/bytes.h copy/move assignment destroys replaced secret
  storage through its zeroizing destructor instead of ordinary vector assignment.

These changes retain upstream notices and are covered by native regression tests.
Install LICENSE and this provenance record with every binary distribution.
