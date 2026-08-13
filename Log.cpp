// Log.cpp - 轻量日志模块实现
#include "pch.h"
#include "Log.h"

#include <cstdarg>
#include <mutex>

namespace wsm {

namespace {
std::mutex g_logMutex;
bool g_inited = false;
std::wstring g_logPath;
} // namespace

void LogInit()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_inited)
        return;

    wchar_t tmp[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH) == 0 || tmp[0] == L'\0') {
        GetTempPathW(MAX_PATH, tmp);
    }
    g_logPath = std::wstring(tmp) + L"\\WindowsSwiftMenu.log";
    g_inited = true;
}

void LogMsg(const wchar_t* fmt, ...)
{
    LogInit();
    std::lock_guard<std::mutex> lock(g_logMutex);

    // 超 4MB 重置, 防止日志无限增长
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 4u * 1024u * 1024u) {
        DeleteFileW(g_logPath.c_str());
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[64] = {};
    swprintf_s(head, L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    wchar_t body[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _TRUNCATE, fmt, ap);
    va_end(ap);

    std::wstring line = std::wstring(head) + body + L"\r\n";

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line.c_str(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
    }
}

} // namespace wsm
