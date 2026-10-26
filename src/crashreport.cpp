#include "crashreport.hpp"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <crtdbg.h>
#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif
#endif

namespace cb2crash {

Live live;

namespace {

std::string gDir, gVersion;
#ifdef _WIN32
char gCrashPath[MAX_PATH + 32] = "";
char gLockPath[MAX_PATH + 48] = "";
char gBuf[24 * 1024];
size_t gLen = 0;
LONG gWriting = 0;

void put(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (gLen < sizeof gBuf - 1) {
        const int n = std::vsnprintf(gBuf + gLen, sizeof gBuf - gLen, fmt, ap);
        if (n > 0) gLen = std::min(sizeof gBuf - 1, gLen + size_t(n));
    }
    va_end(ap);
}

void where(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module) && module) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        std::snprintf(out, size, "%s+0x%llx", base ? base + 1 : path, (unsigned long long)(reinterpret_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    } else {
        std::snprintf(out, size, "0x%llx", (unsigned long long)reinterpret_cast<uintptr_t>(address));
    }
}

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating point divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "page error (a file or mapping became unreadable)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case 0xE06D7363: return "uncaught C++ exception";
    case 0xC0000409: return "fast fail (the runtime detected corruption or called abort)";
    default: return "exception";
    }
}

void header(const char* what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    put("CB2-Online crash report\n========================\n");
    put("what:      %s\n", what);
    put("version:   %s\n", gVersion.c_str());
    put("time:      %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    put("thread:    %lu\n", GetCurrentThreadId());
    put("phase:     %s\n", live.phase);
    put("game:      frame %llu, level %02X, guest pc %08X\n", (unsigned long long)live.frame.load(), live.level.load(), live.guestPc.load());
    if (live.room[0]) put("online:    %s\n", live.room);
    put("graphics:  %s\n", live.renderer);
    put("settings:  %s\n", live.settings);
}

void flushToDisk() {
    if (!gCrashPath[0]) return;
    const HANDLE f = CreateFileA(gCrashPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, gBuf, DWORD(gLen), &written, nullptr);
    CloseHandle(f);
}

#if defined(_M_X64)
void stack(const CONTEXT* from) {
    CONTEXT ctx = *from;
    put("stack:\n");
    for (int i = 0; i < 40 && ctx.Rip; ++i) {
        char w[200];
        where(reinterpret_cast<void*>(ctx.Rip), w, sizeof w);
        put("  %s\n", w);
        DWORD64 imageBase = 0;
        const PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fn) {
            PVOID handlerData = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &frame, nullptr);
        } else {
            if (!ctx.Rsp) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        }
    }
}
#endif

void writeReport(const char* what, const CONTEXT* context, const EXCEPTION_RECORD* record) {
    if (InterlockedExchange(&gWriting, 1)) return;
    gLen = 0;
    header(what);
    if (record) {
        char w[200];
        where(record->ExceptionAddress, w, sizeof w);
        put("code:      0x%08lX (%s) at %s\n", record->ExceptionCode, exceptionName(record->ExceptionCode), w);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            put("access:    %s address 0x%llx\n", record->ExceptionInformation[0] == 0 ? "reading" : record->ExceptionInformation[0] == 1 ? "writing" : "executing", (unsigned long long)record->ExceptionInformation[1]);
    }
#if defined(_M_X64)
    if (context) stack(context);
#else
    (void)context;
#endif
    flushToDisk();
}

LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    writeReport("unhandled exception", ep->ContextRecord, ep->ExceptionRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void die(const char* what) {
    CONTEXT c;
    RtlCaptureContext(&c);
    writeReport(what, &c, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
    std::abort();
}

void onTerminate() {
    const char* what = "std::terminate (an exception nobody caught)";
    static char text[320];
    try {
        if (std::current_exception()) std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
        std::snprintf(text, sizeof text, "std::terminate: uncaught %s", e.what());
        what = text;
    } catch (...) {
    }
    die(what);
}

void onAbort(int) { die("abort() was called"); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) { die("invalid parameter passed to the C runtime"); }
void onPureCall() { die("pure virtual function call"); }

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string osVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (const HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
        if (const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&v);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    char buf[160];
    std::snprintf(buf, sizeof buf, "Windows %lu.%lu build %lu, %lu logical processors, %llu MB RAM", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20));
    return buf;
}
#endif

std::string readFile(const std::string& path, size_t cap) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() > cap) s = s.substr(0, cap) + "\n...";
    return s;
}

}  // namespace

void setPhase(const char* phase) { std::snprintf(live.phase, sizeof live.phase, "%s", phase); }

void install(const std::string& dir, const std::string& version) {
    gDir = dir;
    gVersion = version;
#ifdef _WIN32
    std::snprintf(gCrashPath, sizeof gCrashPath, "%s\\crash.log", dir.c_str());
    SetUnhandledExceptionFilter(filter);
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
#endif
}

void sessionStart() {
#ifdef _WIN32
    std::snprintf(gLockPath, sizeof gLockPath, "%s\\running-%lu.lock", gDir.c_str(), GetCurrentProcessId());
    heartbeat();
#endif
}

void heartbeat() {
#ifdef _WIN32
    if (!gLockPath[0]) return;
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 2000) return;
    last = now;
    char text[700];
    const int n = std::snprintf(text, sizeof text, "version %s\nphase %s\nframe %llu, level %02X, guest pc %08X\ngraphics %s\nsettings %s\n%s%s", gVersion.c_str(), live.phase, (unsigned long long)live.frame.load(),
                                live.level.load(), live.guestPc.load(), live.renderer, live.settings, live.room[0] ? "online " : "", live.room);
    const HANDLE f = CreateFileA(gLockPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, DWORD(n > 0 ? n : 0), &written, nullptr);
    CloseHandle(f);
#endif
}

void sessionEnd() {
#ifdef _WIN32
    if (gLockPath[0]) DeleteFileA(gLockPath);
    gLockPath[0] = 0;
#endif
}

bool previousSessionFailed(std::string& details) {
#ifdef _WIN32
    const std::string crash = gDir + "\\crash.log";
    const std::string archived = gDir + "\\crash-last.log";
    bool failed = false;
    if (GetFileAttributesA(crash.c_str()) != INVALID_FILE_ATTRIBUTES) {
        details = readFile(crash, 12000);
        DeleteFileA(archived.c_str());
        MoveFileA(crash.c_str(), archived.c_str());
        failed = true;
    }
    // sessions that left their lock file behind: the process is gone but never said goodbye
    WIN32_FIND_DATAA fd;
    const HANDLE h = FindFirstFileA((gDir + "\\running-*.lock").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const unsigned long pid = std::strtoul(fd.cFileName + 8, nullptr, 10);
            const std::string path = gDir + "\\" + fd.cFileName;
            bool alive = false;
            if (pid == GetCurrentProcessId()) alive = true;
            else if (const HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) { alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT; CloseHandle(p); }
            if (alive) continue;
            if (!failed) {
                details = "The game ended without a clean exit and without an error report (the process was killed, or the graphics driver or the system stopped it).\nLast known state:\n" + readFile(path, 2000);
                failed = true;
            }
            DeleteFileA(path.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return failed;
#else
    (void)details;
    return false;
#endif
}

std::string reportText(const std::string& crashDetails) {
    std::ostringstream o;
    o << "**What happened**\n(please describe what you were doing)\n\n";
    o << "**Version:** " << gVersion << "\n";
#ifdef _WIN32
    o << "**System:** " << osVersion() << "\n";
#endif
    o << "**Graphics:** " << live.renderer << "\n";
    o << "**Settings:** " << live.settings << "\n";
    o << "**State:** " << live.phase << ", level " << std::hex << std::uppercase << live.level.load() << std::dec << ", frame " << live.frame.load();
    if (live.room[0]) o << ", online " << live.room;
    o << "\n";
    std::string crash = crashDetails;
    const char* title = "Crash details";
    if (crash.empty()) { crash = readFile(gDir + "\\crash-last.log", 6000); title = "Last crash on this machine (may be unrelated)"; }
    if (!crash.empty()) o << "\n**" << title << "**\n```\n" << crash << "\n```\n";
    return o.str();
}

bool openIssue(const std::string& repo, const std::string& title, const std::string& body) {
#ifdef _WIN32
    auto enc = [](const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string r;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += char(c);
            else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
        }
        return r;
    };
    // browsers and Windows refuse very long addresses: shorten the body until it fits
    std::string text = body;
    std::string url;
    for (;;) {
        url = "https://github.com/" + repo + "/issues/new?title=" + enc(title) + "&body=" + enc(text);
        if (url.size() <= 7000 || text.size() < 400) break;
        text = text.substr(0, text.size() * 8 / 10) + "\n...(shortened: crash-last.log next to the game has the full text)";
    }
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    (void)repo; (void)title; (void)body;
    return false;
#endif
}

bool ask(const std::string& title, const std::string& text) {
#ifdef _WIN32
    return MessageBoxW(nullptr, wide(text).c_str(), wide(title).c_str(), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
#else
    (void)title; (void)text;
    return false;
#endif
}

}  // namespace cb2crash
