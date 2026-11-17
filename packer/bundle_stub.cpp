#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace {
constexpr int kMagicBytes = 8;
constexpr int kTrailerBytes = kMagicBytes + static_cast<int>(sizeof(std::uint64_t));

std::wstring quoteArg(const std::wstring &value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') out += L"\\\"";
        else out += ch;
    }
    out += L'"';
    return out;
}

void fail(const std::wstring &message) {
    MessageBoxW(nullptr, message.c_str(), L"Downloader", MB_OK | MB_ICONERROR);
}

std::wstring selfPath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
    path.resize(length);
    return path;
}

bool readPayload(const std::wstring &exePath, std::uint64_t &zipSize, std::uint64_t &zipOffset) {
    HANDLE file = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(file, &size);
    if (!sized || size.QuadPart <= kTrailerBytes) {
        CloseHandle(file);
        return false;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = size.QuadPart - kTrailerBytes;
    if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }
    char magic[kMagicBytes]{};
    DWORD read = 0;
    std::uint64_t payloadSize = 0;
    const bool ok = ReadFile(file, magic, kMagicBytes, &read, nullptr) && read == kMagicBytes &&
                    ReadFile(file, &payloadSize, sizeof(payloadSize), &read, nullptr) && read == sizeof(payloadSize);
    CloseHandle(file);
    if (!ok || std::string(magic, magic + kMagicBytes) != "DWNBNDL1") return false;
    if (payloadSize == 0 || payloadSize + kTrailerBytes > static_cast<std::uint64_t>(size.QuadPart)) return false;
    zipSize = payloadSize;
    zipOffset = static_cast<std::uint64_t>(size.QuadPart) - kTrailerBytes - payloadSize;
    return true;
}

bool writePayloadZip(const std::wstring &exePath, std::uint64_t zipOffset, std::uint64_t zipSize, const std::wstring &zipPath) {
    HANDLE input = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE output = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        return false;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(zipOffset);
    bool ok = SetFilePointerEx(input, pos, nullptr, FILE_BEGIN);
    std::vector<char> buffer(1 << 20);
    std::uint64_t remaining = zipSize;
    while (ok && remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(remaining < buffer.size() ? remaining : buffer.size());
        DWORD read = 0;
        DWORD written = 0;
        ok = ReadFile(input, buffer.data(), chunk, &read, nullptr) && read == chunk &&
             WriteFile(output, buffer.data(), read, &written, nullptr) && written == read;
        remaining -= read;
    }
    CloseHandle(input);
    CloseHandle(output);
    return ok;
}

std::wstring hashName(std::uint64_t zipOffset, std::uint64_t zipSize) {
    wchar_t text[64]{};
    swprintf_s(text, L"%llX-%llX", static_cast<unsigned long long>(zipOffset), static_cast<unsigned long long>(zipSize));
    return text;
}

bool extractZip(const std::wstring &zipPath, const std::wstring &destination) {
    const std::wstring tar = L"C:\\Windows\\System32\\tar.exe";
    const std::wstring command = quoteArg(tar) + L" -xf " + quoteArg(zipPath) + L" -C " + quoteArg(destination);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(tar.c_str(), &mutableCommand[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

bool launch(const std::wstring &application, const std::wstring &directory, DWORD &exitCode) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring command = quoteArg(application);
    for (int i = 1; argv && i < argc; ++i) command += L" " + quoteArg(argv[i]);
    if (argv) LocalFree(argv);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(application.c_str(), &mutableCommand[0], nullptr, nullptr, FALSE, 0, nullptr, directory.c_str(), &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring exe = selfPath();
    std::uint64_t zipSize = 0;
    std::uint64_t zipOffset = 0;
    if (!readPayload(exe, zipSize, zipOffset)) {
        fail(L"This executable does not contain the Downloader runtime payload.");
        return 1;
    }

    wchar_t localAppData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localAppData))) {
        fail(L"Could not locate the local application data folder.");
        return 1;
    }
    const std::wstring runtime = std::wstring(localAppData) + L"\\Downloader\\runtime\\" + hashName(zipOffset, zipSize);
    const std::wstring ready = runtime + L"\\.ready";
    const std::wstring application = runtime + L"\\downloader.exe";
    if (GetFileAttributesW(ready.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(application.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SHCreateDirectoryExW(nullptr, runtime.c_str(), nullptr);
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring zipPath = std::wstring(tempDir) + L"downloader-runtime.zip";
        if (!writePayloadZip(exe, zipOffset, zipSize, zipPath) || !extractZip(zipPath, runtime)) {
            DeleteFileW(zipPath.c_str());
            fail(L"Could not unpack FFmpeg, yt-dlp, and the Downloader runtime.");
            return 1;
        }
        DeleteFileW(zipPath.c_str());
        HANDLE marker = CreateFileW(ready.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    }

    DWORD exitCode = 1;
    if (!launch(application, runtime, exitCode)) {
        fail(L"Could not start the bundled Downloader application.");
        return 1;
    }
    return static_cast<int>(exitCode);
}
