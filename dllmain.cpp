// dllmain.cpp : 重构版 - DllMain + 服务启动
//
// 架构:
//   - 本 DLL 通过 ShellServiceObjectDelayLoad 被 explorer.exe 启动时加载
//   - DllMain(DLL_PROCESS_ATTACH) 检测进程为 explorer 后启动两个线程:
//       1. 钩子线程: WH_MOUSE_LL, 吞掉 explorer/桌面上的右键
//       2. 菜单线程: 创建自绘菜单窗口(Direct2D + DComp 透明合成 + DWM 亚克力)
//   - 进程内弹菜单, 零跨进程延迟; 无 XAML, GPU 加速手绘
//   - 真实菜单数据由 ContextMenuHost 从 shell 的 IContextMenu 枚举

#include "pch.h"
#include "framework.h"
#include "dllmain.h"
#include "HookManager.h"
#include "MenuWindow.h"
#include "Log.h"

CSwiftMenuModule _AtlModule;

// OBJECT_ENTRY_AUTO: SSODL 服务对象 (链接器 ATL$__m 段自注册)
OBJECT_ENTRY_AUTO(CLSID_SwiftMenuService, CServiceObject)

namespace {

// 当前进程是否为 explorer.exe
bool IsExplorerProcess()
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return false;
    wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"explorer.exe") == 0;
}

} // namespace

// DLL 入口点
extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        // 加载留痕(原始 API, loader lock 安全)
        wchar_t logPath[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, logPath) > 0) {
            wcscat_s(logPath, L"WindowsSwiftMenu.loadlog");
            HANDLE hLog = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hLog != INVALID_HANDLE_VALUE) {
                wchar_t line[256] = {};
                wsprintfW(line, L"[%lu] DLL_PROCESS_ATTACH pid=%lu\n",
                          GetTickCount(), GetCurrentProcessId());
                DWORD written = 0;
                WriteFile(hLog, line, (DWORD)(wcslen(line) * sizeof(wchar_t)),
                          &written, nullptr);
                CloseHandle(hLog);
            }
        }

        // 仅在 explorer 进程内启动服务
        if (IsExplorerProcess()) {
            // DllMain 内不能等待线程, 只创建
            wsm::LogInit();
            WSM_LOG(L"[v4] DLL 已加载到 explorer, 启动服务");
            wsm::HookManager::Start();
            wsm::MenuWindow::Start();
        }
    }
    if (dwReason == DLL_PROCESS_DETACH) {
        // 不能等待线程; 通过原子标志请求退出
        wsm::HookManager::StopRequest();
        wsm::MenuWindow::StopRequest();
    }
    return _AtlModule.DllMain(dwReason, lpReserved);
}
