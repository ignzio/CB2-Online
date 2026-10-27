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
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
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
char gBuf[56 * 1024];
size_t gLen = 0;
LONG gWriting = 0;

// ---- the session log ----
HANDLE gLog = INVALID_HANDLE_VALUE;
char gLogPath[MAX_PATH + 48] = "", gPrevLogPath[MAX_PATH + 64] = "";
ULONGLONG gStartTick = 0;
CRITICAL_SECTION gLogLock;
bool gLogLockReady = false;
DWORD gLogBytes = 0;
constexpr DWORD kLogCap = 1u << 20;   // a session never writes more than 1 MB of events
std::atomic<uint64_t> gBeat{0};
DWORD gMainThreadId = 0;
HANDLE gMainThread = nullptr;
std::atomic<bool> gMonitorOn{false};

// The last bytes of a file, from the first complete line on, into `out` (NUL terminated). Plain Win32 calls: it is also used while the process is crashing.
size_t readTail(const char* path, char* out, size_t cap) {
    out[0] = 0;
    const HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG want = std::min<LONGLONG>(size.QuadPart, LONGLONG(cap - 1));
    LARGE_INTEGER pos;
    pos.QuadPart = size.QuadPart - want;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    DWORD got = 0;
    ReadFile(f, out, DWORD(want), &got, nullptr);
    CloseHandle(f);
    out[got] = 0;
    size_t skip = 0;
    if (want < size.QuadPart) { while (skip < got && out[skip] != '\n') ++skip; if (skip < got) ++skip; }   // the first line is probably cut
    if (skip) std::memmove(out, out + skip, got - skip + 1);
    return got - skip;
}

void logWrite(const char* text, size_t n) {
    if (gLog == INVALID_HANDLE_VALUE || !n) return;
    DWORD w = 0;
    if (gLogBytes + n <= kLogCap) { WriteFile(gLog, text, DWORD(n), &w, nullptr); gLogBytes += w; }
    else if (gLogBytes < kLogCap + 64) { static const char full[] = "(the log is full: the rest of this session is not recorded)\r\n"; WriteFile(gLog, full, DWORD(sizeof full - 1), &w, nullptr); gLogBytes = kLogCap + 64; }
}

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
// The call stack of `from` as "module+0xRVA" lines (unwound with the compiler's tables) appended to out.
void stackInto(const CONTEXT* from, char* out, size_t cap) {
    CONTEXT ctx = *from;
    size_t len = std::strlen(out);
    for (int i = 0; i < 40 && ctx.Rip; ++i) {
        char w[200];
        where(reinterpret_cast<void*>(ctx.Rip), w, sizeof w);
        const int n = std::snprintf(out + len, cap - len, "  %s\n", w);
        if (n <= 0 || size_t(n) >= cap - len) break;
        len += size_t(n);
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

void stack(const CONTEXT* from) {
    static char text[4096];
    text[0] = 0;
    stackInto(from, text, sizeof text);
    put("stack:\n%s", text);
}
#endif

// What was loaded into the process (security programs and overlays inject their DLLs; graphics drivers show here too): names only.
void modules() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), 512);
    put("modules (%zu):", count);
    size_t shown = 0;
    for (size_t i = 0; i < count && gLen < sizeof gBuf - 9000; ++i) {
        char path[MAX_PATH] = "";
        GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        put(" %s", base ? base + 1 : path);
        ++shown;
    }
    put("\n");
}
void modulesSafe() { __try { modules(); } __except (EXCEPTION_EXECUTE_HANDLER) { put("(the module list could not be read)\n"); } }

void processFacts() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        put("memory:    working set %llu MB (peak %llu MB), committed %llu MB\n", (unsigned long long)(pmc.WorkingSetSize >> 20), (unsigned long long)(pmc.PeakWorkingSetSize >> 20), (unsigned long long)(pmc.PagefileUsage >> 20));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    put("objects:   %lu handles, %lu GDI, %lu USER\n", handles, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    put("uptime:    %.1f s\n", double(GetTickCount64() - gStartTick) / 1000.0);
}

// A minidump (open it in Visual Studio or WinDbg with the matching Crash2.map / symbols). dbghelp is loaded only now, so a normal run never touches it.
void writeDump(const char* path, EXCEPTION_POINTERS* ep, DWORD threadId) {
    const HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) return;
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    const auto fn = reinterpret_cast<DumpFn>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!fn) return;
    const HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = threadId;
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData), ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
}
void writeDumpSafe(const char* path, EXCEPTION_POINTERS* ep, DWORD threadId) { __try { writeDump(path, ep, threadId); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

void writeReport(const char* what, const CONTEXT* context, const EXCEPTION_RECORD* record) {
    if (InterlockedExchange(&gWriting, 1)) return;
    gLen = 0;
    header(what);
    processFacts();
    if (record) {
        char w[200];
        where(record->ExceptionAddress, w, sizeof w);
        put("code:      0x%08lX (%s) at %s\n", record->ExceptionCode, exceptionName(record->ExceptionCode), w);
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
            put("access:    %s address 0x%llx\n", record->ExceptionInformation[0] == 0 ? "reading" : record->ExceptionInformation[0] == 1 ? "writing" : "executing", (unsigned long long)record->ExceptionInformation[1]);
    }
#if defined(_M_X64)
    if (context) {
        stack(context);
        put("registers: rip %016llx rsp %016llx rax %016llx rcx %016llx rdx %016llx rbx %016llx rsi %016llx rdi %016llx\n", context->Rip, context->Rsp, context->Rax, context->Rcx, context->Rdx, context->Rbx, context->Rsi, context->Rdi);
    }
#else
    (void)context;
#endif
    modulesSafe();
    {   // the last events of the session: what led here
        static char tail[8192];
        if (readTail(gLogPath, tail, sizeof tail)) put("recent events (cb2-session.log):\n%s", tail);
    }
    put("\nFiles next to the game that help: crash.log (this), cb2-session.log, crash.dmp. See docs/CRASHES.md.\n");
    flushToDisk();
    if (gLog != INVALID_HANDLE_VALUE) { char line[200]; const int n = std::snprintf(line, sizeof line, "CRASH: %s\r\n", what); logWrite(line, size_t(n > 0 ? n : 0)); }
}

LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    writeReport("unhandled exception", ep->ContextRecord, ep->ExceptionRecord);
    char dump[MAX_PATH + 32];
    std::snprintf(dump, sizeof dump, "%s\\crash.dmp", gDir.c_str());
    writeDumpSafe(dump, ep, GetCurrentThreadId());
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void die(const char* what) {
    CONTEXT c;
    RtlCaptureContext(&c);
    writeReport(what, &c, nullptr);
    char dump[MAX_PATH + 32];
    std::snprintf(dump, sizeof dump, "%s\\crash.dmp", gDir.c_str());
    writeDumpSafe(dump, nullptr, GetCurrentThreadId());
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

void setPhase(const char* phase) {
    if (std::strcmp(live.phase, phase) != 0) {
#ifdef _WIN32
        log("phase: %s", phase);
#endif
        std::snprintf(live.phase, sizeof live.phase, "%s", phase);
    }
}

void log(const char* fmt, ...) {
#ifdef _WIN32
    if (gLog == INVALID_HANDLE_VALUE || !gLogLockReady) return;
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1000];
    const int n = std::snprintf(line, sizeof line, "[%02d:%02d:%02d +%7.1fs] %s\r\n", t.wHour, t.wMinute, t.wSecond, double(GetTickCount64() - gStartTick) / 1000.0, body);
    if (n <= 0) return;
    EnterCriticalSection(&gLogLock);
    logWrite(line, size_t(std::min<int>(n, int(sizeof line) - 1)));
    LeaveCriticalSection(&gLogLock);
#else
    (void)fmt;
#endif
}

std::string recentEvents(size_t maxBytes) {
#ifdef _WIN32
    std::vector<char> buf(std::min<size_t>(maxBytes, 64 * 1024) + 1);
    readTail(gLogPath, buf.data(), buf.size());
    return buf.data();
#else
    (void)maxBytes;
    return {};
#endif
}

void beat() { gBeat.fetch_add(1, std::memory_order_relaxed); }

void status() {
#ifdef _WIN32
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 30000) return;
    last = now;
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    log("alive: frame %llu, level %02X, %s, memory %llu MB (peak %llu), %lu handles, %lu GDI objects", (unsigned long long)live.frame.load(), live.level.load(), live.phase,
        (unsigned long long)(pmc.WorkingSetSize >> 20), (unsigned long long)(pmc.PeakWorkingSetSize >> 20), handles, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
#endif
}

#ifdef _WIN32
namespace {
DWORD WINAPI monitorMain(LPVOID) {
    uint64_t last = gBeat.load();
    ULONGLONG changed = GetTickCount64();
    bool reported = false;
    while (gMonitorOn) {
        Sleep(2000);
        const uint64_t b = gBeat.load();
        const ULONGLONG now = GetTickCount64();
        if (b != last) {
            if (reported) log("the main loop is running again after %.0f s", double(now - changed) / 1000.0);
            reported = false;
            last = b;
            changed = now;
            continue;
        }
        if (reported || now - changed < 20000 || !gMainThread) continue;
        reported = true;
        // where is the main thread? (it is stopped for a moment to read its registers)
        static char text[4096];
        text[0] = 0;
        bool haveStack = false;
        if (SuspendThread(gMainThread) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(gMainThread, &ctx)) {
#if defined(_M_X64)
                stackInto(&ctx, text, sizeof text);
                haveStack = true;
#endif
            }
            ResumeThread(gMainThread);
        }
        log("NOT RESPONDING: the main loop has not run for %.0f s (frame %llu, level %02X, guest pc %08X, phase %s). It may be a window being dragged, a dialog, or a real hang.", double(now - changed) / 1000.0,
            (unsigned long long)live.frame.load(), live.level.load(), live.guestPc.load(), live.phase);
        if (haveStack) log("main thread stack:\n%s", text);
        char dump[MAX_PATH + 32];
        std::snprintf(dump, sizeof dump, "%s\\hang.dmp", gDir.c_str());
        writeDumpSafe(dump, nullptr, 0);
        log("hang.dmp written");
    }
    return 0;
}
}  // namespace
#endif

void watchMainThread() {
#ifdef _WIN32
    if (gMonitorOn.exchange(true)) return;
    gMainThreadId = GetCurrentThreadId();
    gMainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, gMainThreadId);
    if (const HANDLE th = CreateThread(nullptr, 256 * 1024, monitorMain, nullptr, 0, nullptr)) CloseHandle(th);
    else gMonitorOn = false;
#endif
}

void showCrashFiles() {
#ifdef _WIN32
    const std::string file = gDir + "\\crash-last.log";
    const std::wstring args = L"/select,\"" + wide(GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES ? file : gDir) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#endif
}

void install(const std::string& dir, const std::string& version) {
    gDir = dir;
    gVersion = version;
#ifdef _WIN32
    std::snprintf(gCrashPath, sizeof gCrashPath, "%s\\crash.log", dir.c_str());
    gStartTick = GetTickCount64();
    {   // the session log: the previous one is kept (a crash is looked at by the next start); written line by line so that nothing is lost when the process is killed
        std::snprintf(gLogPath, sizeof gLogPath, "%s\\cb2-session.log", dir.c_str());
        std::snprintf(gPrevLogPath, sizeof gPrevLogPath, "%s\\cb2-session-previous.log", dir.c_str());
        MoveFileExA(gLogPath, gPrevLogPath, MOVEFILE_REPLACE_EXISTING);
        InitializeCriticalSection(&gLogLock);
        gLogLockReady = true;
        gLog = CreateFileA(gLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        // a program without a console has no stderr: what the game prints there goes to the log instead
        const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if (gLog != INVALID_HANDLE_VALUE && (!err || err == INVALID_HANDLE_VALUE)) {
            FILE* f = nullptr;
            if (freopen_s(&f, gLogPath, "a", stderr) == 0) std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
        char cmd[600];
        std::snprintf(cmd, sizeof cmd, "%s", GetCommandLineA());
        log("CB2-Online %s started", version.c_str());
        log("system: %s", osVersion().c_str());
        log("command line: %s", cmd);
        log("game folder: %s", dir.c_str());
    }
    ULONG guarantee = 96 * 1024;   // room for the crash handler to run after a stack overflow
    SetThreadStackGuarantee(&guarantee);
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
    gMonitorOn = false;
    if (gWriting) return;   // a crash report was written: the lock file stays, and this is not a clean end
    if (gLockPath[0]) log("session ended cleanly");
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
        details = readFile(crash, 16000);
        DeleteFileA(archived.c_str());
        MoveFileA(crash.c_str(), archived.c_str());
        failed = true;
    }
    if (failed || [&] { WIN32_FIND_DATAA f; const HANDLE h = FindFirstFileA((gDir + "\\running-*.lock").c_str(), &f); if (h == INVALID_HANDLE_VALUE) return false; FindClose(h); return true; }()) {
        // keep what the session before this one left (this start has already rotated the logs), so a second start does not overwrite it
        CopyFileA(gPrevLogPath, (gDir + "\\crash-last-session.log").c_str(), FALSE);
        if (GetFileAttributesA((gDir + "\\crash.dmp").c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileA((gDir + "\\crash-last.dmp").c_str()); MoveFileA((gDir + "\\crash.dmp").c_str(), (gDir + "\\crash-last.dmp").c_str()); }
        if (GetFileAttributesA((gDir + "\\hang.dmp").c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileA((gDir + "\\hang-last.dmp").c_str()); MoveFileA((gDir + "\\hang.dmp").c_str(), (gDir + "\\hang-last.dmp").c_str()); }
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
                static char tail[6000];
                readTail(gPrevLogPath, tail, sizeof tail);
                details = "The game ended without a clean exit and without an error report (the process was killed, or the graphics driver or the system stopped it).\nLast known state:\n" + readFile(path, 2000) +
                          "\nLast events of that session:\n" + tail;
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
    else {
        const std::string events = recentEvents(3500);
        if (!events.empty()) o << "\n**Recent events of this session**\n```\n" << events << "\n```\n";
    }
    o << "\n_Files next to the game that help (zip them and drag the zip into this issue): `crash-last.log`, `crash-last-session.log`, `cb2-session.log`, `crash-last.dmp` / `hang-last.dmp`._\n";
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
