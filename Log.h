// Log.h - 轻量日志模块
// 日志文件: %TEMP%\WindowsSwiftMenu.log (UTF-16LE, 追加模式, 超 4MB 自动重置)
#pragma once

#include <windows.h>
#include <string>

namespace wsm {

// 初始化日志(幂等)。通常在 Service 启动时调用。
void LogInit();

// 写一条日志。printf 风格宽字符格式。
void LogMsg(const wchar_t* fmt, ...);

} // namespace wsm

#define WSM_LOG(...) ::wsm::LogMsg(__VA_ARGS__)
