#include "archive.h"
#include <algorithm>
#include <fstream>
#include <iostream>

template<class T> void write(std::ofstream &out, T value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}
int wmain(int argc, wchar_t **argv) {
    try {
        require(argc == 3, "Usage: pack-runtime RUNTIME_DIR OUTPUT");
        const fs::path root = fs::absolute(argv[1]);
        require(fs::is_regular_file(root / "bin/VideoPlayer.exe"), "Missing player executable");
        std::vector<fs::path> files;
        for (const auto &entry : fs::recursive_directory_iterator(root)) {
            require(!entry.is_symlink(), "Runtime must not contain symlinks");
            if (entry.is_regular_file()) files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        std::ofstream out(fs::path(argv[2]), std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        write(out, archiveMagic);
        write(out, static_cast<uint32_t>(files.size()));
        Codec codec(true);
        for (const auto &file : files) {
            const auto path = file.lexically_relative(root).generic_u8string();
            std::vector<char> bytes(static_cast<size_t>(fs::file_size(file)));
            std::ifstream in(file, std::ios::binary);
            in.exceptions(std::ios::badbit | std::ios::failbit);
            if (!bytes.empty()) in.read(bytes.data(), bytes.size());
            SIZE_T size = 0;
            std::vector<char> compressed;
            if (!bytes.empty()) {
                Compress(codec.handle, bytes.data(), bytes.size(), nullptr, 0, &size);
                require(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Cannot size compressed file");
                compressed.resize(size);
                require(Compress(codec.handle, bytes.data(), bytes.size(), compressed.data(), size, &size), "Compression failed");
                compressed.resize(size);
            }
            write(out, static_cast<uint32_t>(path.size()));
            write(out, static_cast<uint64_t>(bytes.size()));
            write(out, static_cast<uint64_t>(compressed.size()));
            out.write(path.data(), path.size());
            if (!compressed.empty()) out.write(compressed.data(), compressed.size());
        }
        std::cout << "Packed " << files.size() << " runtime files\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
