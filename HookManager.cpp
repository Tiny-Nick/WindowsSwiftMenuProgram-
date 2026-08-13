// HookManager.cpp - v4 全局鼠标钩子实现
//
// 在 explorer 进程内安装 WH_MOUSE_LL 钩子:
//   - 右键落在资源管理器/桌面 -> 吞掉事件(原生菜单永不出现),
//     通知菜单线程在鼠标位置显示自绘菜单
//   - 左键落在自绘菜单外 -> 通知关闭菜单(左键放行)
//
// 重要: 低层钩子回调必须极快(系统超时会移除钩子), 回调内禁止文件 IO,
//       禁止 Log 写盘, 只做内存操作 + PostMessage。

#include "pch.h"
#include "HookManager.h"

#include "Log.h"
#include "MenuWindow.h"

namespace wsm {

namespace {

HHOOK g_hook = nullptr;
HANDLE g_thread = nullptr;
volatile LONG g_running = 0;
HINSTANCE g_hMod = nullptr;

// 目标顶层窗口类白名单
bool IsTargetWindow(HWND hwnd)
{
    wchar_t cls[64] = {};
    if (!hwnd || GetClassNameW(hwnd, cls, 64) == 0)
        return false;
    return wcscmp(cls, L"CabinetWClass") == 0 ||
           wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode != HC_ACTION)
        return CallNextHookEx(nullptr, nCode, wp, lp);

    const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);

    // 左键: 菜单可见且点击在菜单外 -> 关闭菜单(左键放行)
    if (wp == WM_LBUTTONDOWN && MenuWindow::IsVisible()) {
        RECT rc = {};
        GetWindowRect(MenuWindow::GetHwnd(), &rc);
        if (!PtInRect(&rc, ms->pt))
            MenuWindow::CloseRequest();
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }

    if (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) {
        // 取鼠标下窗口的顶层祖先(文件列表是 SysListView32 子窗口)
        HWND root = GetAncestor(WindowFromPoint(ms->pt), GA_ROOT);
        if (IsTargetWindow(root)) {
            if (wp == WM_RBUTTONDOWN) {
                HWND hMenu = MenuWindow::GetHwnd();
                if (hMenu) {
                    // 完整 32 位坐标 (不用 MAKELPARAM, 避免多显示器/负坐标截断)
                    PostMessageW(hMenu, WM_APP + 2,
                                 static_cast<WPARAM>(static_cast<LONG_PTR>(ms->pt.x)),
                                 static_cast<LPARAM>(static_cast<LONG_PTR>(ms->pt.y)));
                }
            }
            return 1; // 吞掉右键
        }
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

DWORD WINAPI HookThreadProc(LPVOID)
{
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_hMod, 0);
    if (!g_hook) {
        // 失败也要写日志, 但注意此时不能在回调里写(回调不写), 这里在
        // 线程上下文写是安全的
        WSM_LOG(L"[Hook] SetWindowsHookEx 失败 err=%lu", GetLastError());
        return 1;
    }
    WSM_LOG(L"[Hook] 钩子已安装 (explorer 进程内)");

    MSG msg;
    while (InterlockedCompareExchange(&g_running, 1, 1) &&
           GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    WSM_LOG(L"[Hook] 钩子已卸载");
    return 0;
}

} // namespace

void HookManager::Start()
{
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0)
        return; // 已启动
    g_hMod = GetModuleHandleW(L"WindowsSwiftMenu.dll");
    g_thread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        WSM_LOG(L"[Hook] 线程创建失败 err=%lu", GetLastError());
        InterlockedExchange(&g_running, 0);
    }
}

void HookManager::StopRequest()
{
    InterlockedExchange(&g_running, 0);
    if (g_thread) {
        PostThreadMessageW(GetThreadId(g_thread), WM_QUIT, 0, 0);
        // DllMain 中不能等待, 交给进程退出清理
    }
}

} // namespace wsm
