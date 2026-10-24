#include "relaunch.hpp"

#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace cb2 {

namespace {

std::string quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t backslashes = 0;
    for (char c : a) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') q.append(backslashes * 2 + 1, '\\');
        else q.append(backslashes, '\\');
        backslashes = 0;
        q += c;
    }
    q.append(backslashes * 2, '\\');
    return q + "\"";
}

}   // namespace

bool relaunchToLobby(int argc, char** argv) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)))) return false;
    int len = WideCharToMultiByte(CP_UTF8, 0, exe, -1, nullptr, 0, nullptr, nullptr);
    std::string exe8(size_t(len > 0 ? len - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe8.data(), len, nullptr, nullptr);

    static const char* withValue[] = {"--room", "--directory", "--rendezvous", "--menu-at", "--menu-script", "--lobby-script", "--lobby-shot", "--capture", "--press", "--pace-from", "--capture-after-puppet"};
    static const char* flagOnly[] = {"--online", "--no-lobby", "--lobby", "--autostart", "--hidden", "--window-capture"};
    std::string line = quote(exe8);
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        bool drop = false;
        for (const char* f : withValue) if (!std::strcmp(a, f)) { drop = true; ++i; break; }
        if (!drop && !std::strcmp(a, "--fake-player")) { drop = true; if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') ++i; }
        if (!drop) for (const char* f : flagOnly) if (!std::strcmp(a, f)) { drop = true; break; }
        if (!drop && i < argc) line += " " + quote(a);
    }
    line += " --lobby";

    int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
    std::wstring wline(size_t(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wline.data(), wlen);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, wline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)argc; (void)argv;
    return false;
#endif
}

}   // namespace cb2
