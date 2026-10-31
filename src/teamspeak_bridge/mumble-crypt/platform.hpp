#pragma once
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <openssl/rand.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

inline std::uint64_t squadMumbleSwap64(std::uint64_t value) {
    if constexpr (std::endian::native == std::endian::big) return value;
    std::uint64_t result = 0;
    for (int i = 0; i < 8; ++i) { result = (result << 8) | (value & 255); value >>= 8; }
    return result;
}
#define SWAP64(value) squadMumbleSwap64(value)

inline void randomBytes(unsigned char* output, int size) {
    if (RAND_bytes(output, size) != 1) throw std::runtime_error("Mumble random generation failed");
}
