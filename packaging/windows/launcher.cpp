#include "archive.h"
#include "payload.h"
#include <shlobj.h>
#include <shellapi.h>
#include <cstring>
#include <fstream>
#include <string>

struct Handle {
    HANDLE value;
    ~Handle() { if (value) CloseHandle(value); }
};
struct Reader {
    const char *data;
    size_t left;
    const char *take(size_t size) {
        require(size <= left, "Incomplete embedded runtime");
        const char *result = data;
        data += size;
        left -= size;
        return result;
    }
    template<class T> T read() {
        T value;
        std::memcpy(&value, take(sizeof(T)), sizeof(T));
        return value;
    }
};

static void unpack(const fs::path &root) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(1), RT_RCDATA);
    require(resource != nullptr, "Missing embedded runtime");
    const DWORD size = SizeofResource(nullptr, resource);
    const auto data = static_cast<const char *>(LockResource(LoadResource(nullptr, resource)));
    require(data != nullptr, "Cannot read embedded runtime");
    Reader reader{data, size};
    require(reader.read<uint32_t>() == archiveMagic, "Invalid embedded runtime");
    const auto count = reader.read<uint32_t>();
    Codec codec(false);
    for (uint32_t i = 0; i < count; ++i) {
        const auto nameSize = reader.read<uint32_t>();
        const auto rawSize = reader.read<uint64_t>();
        const auto packedSize = reader.read<uint64_t>();
        require(nameSize > 0 && nameSize < 32768 && rawSize < 1024ULL * 1024 * 1024
                && packedSize <= reader.left, "Invalid runtime file size");
        const char *name = reader.take(nameSize);
        require(std::memchr(name, 0, nameSize) == nullptr, "Invalid runtime filename");
        const fs::path relative = fs::u8path(name, name + nameSize);
        require(!relative.has_root_path(), "Invalid runtime path");
        for (const auto &part : relative)
            require(part != ".." && part != "." && part.wstring().find(L':') == std::wstring::npos, "Invalid runtime path");
        const char *packed = reader.take(static_cast<size_t>(packedSize));
        std::vector<char> bytes(static_cast<size_t>(rawSize));
        if (rawSize) {
            SIZE_T written = 0;
            require(Decompress(codec.handle, const_cast<char *>(packed), static_cast<SIZE_T>(packedSize), bytes.data(), bytes.size(), &written)
                    && written == rawSize, "Cannot decompress embedded runtime");
        } else require(packedSize == 0, "Invalid empty runtime file");
        const fs::path target = root / relative;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        if (!bytes.empty()) out.write(bytes.data(), bytes.size());
    }
    require(reader.left == 0, "Unexpected embedded runtime data");
}

// Windows command-line quoting, including embedded quotes and trailing backslashes.
static std::wstring quote(const std::wstring &arg) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        PWSTR local = nullptr;
        require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)), "Cannot locate user cache");
        const fs::path root = fs::path(local) / L"VideoPlayer" / L"Runtime" / PAYLOAD_HASH;
        CoTaskMemFree(local);
        // A file lock is scoped to this user's cache and released even if extraction crashes.
        fs::create_directories(root.parent_path());
        Handle lock{CreateFileW((root.wstring() + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(lock.value != INVALID_HANDLE_VALUE, "Cannot open runtime cache lock");
        OVERLAPPED overlap{};
        require(LockFileEx(lock.value, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlap), "Cannot lock runtime cache");
        if (!fs::exists(root / L"complete") || !fs::exists(root / L"bin/VideoPlayer.exe")) {
            fs::create_directories(root);
            unpack(root);
            std::ofstream marker(root / L"complete");
            marker.exceptions(std::ios::badbit | std::ios::failbit);
            marker << "complete\n";
            marker.close();
        }
        UnlockFileEx(lock.value, 0, 1, 0, &overlap);

        const fs::path executable = root / L"bin/VideoPlayer.exe";
        std::wstring command = quote(executable.wstring());
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        require(argv != nullptr, "Cannot read command line");
        for (int i = 1; i < argc; ++i) command += L" " + quote(argv[i]);
        LocalFree(argv);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        std::vector<wchar_t> launcherPath(32768);
        const DWORD launcherLength = GetModuleFileNameW(nullptr, launcherPath.data(), static_cast<DWORD>(launcherPath.size()));
        require(launcherLength > 0 && launcherLength < launcherPath.size(), "Cannot locate portable launcher");
        require(SetEnvironmentVariableW(L"VIDEO_PLAYER_LAUNCHER", launcherPath.data()), "Cannot pass portable launcher path");
        // Inherit the caller's working directory so relative video filenames still work.
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                               0, nullptr, nullptr, &startup, &process), "Cannot start Video Player");
        Handle child{process.hProcess}, thread{process.hThread};
        AllowSetForegroundWindow(process.dwProcessId);
        WaitForSingleObject(child.value, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(child.value, &code);
        return static_cast<int>(code);
    } catch (const std::exception &e) {
        MessageBoxA(nullptr, e.what(), "Video Player", MB_OK | MB_ICONERROR);
        return 1;
    }
}
