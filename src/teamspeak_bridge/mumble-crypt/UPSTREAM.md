Mumble UDP cryptography
=======================

Source: https://github.com/mumble-voip/mumble/tree/v1.5.939/src/crypto

CryptStateOCB2.cpp, CryptStateOCB2.h and CryptState.h retain the upstream
OCB2 protection, packet authentication, replay window and nonce handling.
The BSD license is in LICENSE.

Local portability changes replace Qt integer types, copy/override macros,
byte swapping and random generation with standard C++ and OpenSSL. The two
unused statistics timers and their single restart were removed. Cipher and
packet-state behavior is unchanged. platform.hpp and bridge.cpp are the
application integration. Do not substitute the older Rust crate cipher.
