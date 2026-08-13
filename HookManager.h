// HookManager.h - v4 全局鼠标钩子 (explorer 进程内)
#pragma once

#include <windows.h>

namespace wsm {

class HookManager {
public:
    // 启动钩子线程 (DllMain 中调用, 不等待)
    static void Start();
    // 请求退出 (DllMain DETACH 中调用, 不等待)
    static void StopRequest();
};

} // namespace wsm
