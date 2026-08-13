// ContextMenuHost.h - 真实 shell 右键菜单数据源
//
// 从 shell 的 IContextMenu 枚举真实菜单项 (文字/命令/子菜单/分隔线/禁用),
// 填充 MenuModel; 点击时通过 IContextMenu::InvokeCommand 真实执行。
#pragma once

#include <windows.h>
#include "MenuModel.h"

namespace wsm {

class ContextMenuHost {
public:
    // 在屏幕 (screenX, screenY) 右键处构建真实菜单模型。
    // parentHwnd 用于 QueryContextMenu/InvokeCommand 的宿主窗口。
    // 成功返回 true (菜单非空), 失败返回 false。
    static bool Build(int screenX, int screenY, HWND parentHwnd, MenuModel& out);

    // 执行一个菜单项 (按 commandId 走 InvokeCommand)
    static void Invoke(const MenuItem& item);
};

} // namespace wsm
