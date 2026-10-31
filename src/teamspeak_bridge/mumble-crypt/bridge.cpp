#include "CryptStateOCB2.h"
#include <algorithm>
#include <memory>

extern "C" {
void* squad_mumble_crypt_new(const unsigned char* key, const unsigned char* encrypt, const unsigned char* decrypt) noexcept {
    try {
        auto state = std::make_unique<CryptStateOCB2>();
        if (!state->setKey({reinterpret_cast<const char*>(key), 16},
                          {reinterpret_cast<const char*>(encrypt), 16},
                          {reinterpret_cast<const char*>(decrypt), 16})) return nullptr;
        return state.release();
    } catch (...) { return nullptr; }
}
void squad_mumble_crypt_free(void* state) noexcept { delete static_cast<CryptStateOCB2*>(state); }
bool squad_mumble_crypt_nonce(void* state, unsigned char* nonce, bool set) noexcept {
    try {
        auto& crypt = *static_cast<CryptStateOCB2*>(state);
        if (set) return crypt.setDecryptIV({reinterpret_cast<const char*>(nonce), 16});
        const auto current = crypt.getEncryptIV();
        if (current.size() != 16) return false;
        std::copy(current.begin(), current.end(), nonce);
        return true;
    } catch (...) { return false; }
}
bool squad_mumble_crypt_packet(void* state, const unsigned char* input, unsigned char* output,
                               unsigned int length, bool encrypt) noexcept {
    try {
        auto& crypt = *static_cast<CryptStateOCB2*>(state);
        return encrypt ? crypt.encrypt(input, output, length) : crypt.decrypt(input, output, length);
    } catch (...) { return false; }
}
}
