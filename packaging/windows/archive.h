#pragma once
#include <windows.h>
#include <compressapi.h>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
constexpr uint32_t archiveMagic = 0x31505641; // AVP1, little endian
inline void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
struct Codec {
    COMPRESSOR_HANDLE handle = nullptr;
    bool compress;
    explicit Codec(bool packing) : compress(packing) {
        require(packing ? CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle)
                        : CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &handle),
                "Cannot initialize Windows compression");
    }
    ~Codec() { if (compress) CloseCompressor(handle); else CloseDecompressor(handle); }
};
