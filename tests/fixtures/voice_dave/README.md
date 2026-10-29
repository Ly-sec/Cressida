# DAVE test data provenance

The fixed media vector is embedded in [voice_dave_test.cpp](../../voice_dave_test.cpp).
It uses synthetic all-zero AES-128 key, 96-bit nonce and 16-byte plaintext,
the standard AES-GCM ciphertext/tag case, and an independently specified DAVE
footer. The test compares exact bytes before testing our parser's inverse.
AES-GCM is specified in [NIST SP 800-38D](https://doi.org/10.6028/NIST.SP.800-38D).

The media ciphertext is `0388dace60b6a392f328c2b971b2fe78`. The protocol retains
the first eight authentication-tag bytes, `ab6e47d42cec13bd`, followed by the
ULEB128 nonce zero, supplemental size twelve and marker bytes `fafa`.
No private or live-call material was captured.

The group fixtures in [voice_encryption_test.cpp](../../voice_encryption_test.cpp)
use generated ephemeral keys, synthetic numeric identities and a local signed
external sender. They do not store credentials or depend on deterministic
production entropy. Parser mutation cases use synthetic data and a fixed
noncryptographic mutation seed only in tests.

[voice_dave_reference_test.cpp](../../voice_dave_reference_test.cpp) is the
optional interoperability oracle. The locally checked reference revision is
`6396c90aedbb9a6ee117ea9ce1ce9832af6a3375`, using the public libdave C API.
Its own MLS namespace differs from Cressida's private bundled namespace.
Both creator/joiner directions and three-member epoch changes exchange genuine
signed handshake objects and encrypted Opus-shaped data in memory.

This comparison needs an explicitly supplied reference installation and
`-Ddave_reference=enabled`. It is disabled for normal builds. The test wrapper
and these synthetic fixtures are project-owned code under the application
license; the external reference library retains its own notices.
Bundled MLS++ source provenance is recorded [separately](../../../third_party/mlspp/PROVENANCE.md).
