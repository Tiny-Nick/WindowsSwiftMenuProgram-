// MenuWindow.h - 自绘右键菜单窗口 (Direct2D + DirectComposition + 亚克力)
#pragma once

#include <windows.h>
#include "MenuModel.h"

namespace wsm {

// 菜单窗口服务 (单例, 运行在独立 STA 菜单线程)
// 钩子线程通过 ShowAt / CloseRequest 异步触发。
class MenuWindow {
public:
    // 启动菜单线程 (DllMain ATTACH 中调用, 不等待)
    static void Start();
    // 请求退出 (DllMain DETACH 中调用, 不等待)
    static void StopRequest();

    // 主菜单窗口句柄 (钩子线程查询用)
    static HWND GetHwnd();
    // 主菜单当前是否可见
    static bool IsVisible();

    // 钩子线程调用: 请求在 (x, y) 弹出菜单 (菜单线程内构建数据 + 显示)
    static void ShowAt(int x, int y);
    // 钩子线程调用: 请求关闭全部菜单
    static void CloseRequest();
};

} // namespace wsm
