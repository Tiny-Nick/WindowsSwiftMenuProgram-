// MenuModel.h - 菜单数据模型 (渲染与真实 shell 数据解耦)
//
// MenuItem   : 单个菜单项 (含子菜单)
// MenuGroup  : 一个"卡片"(圆角矩形分组)
// MenuModel  : 完整菜单 = 顶部横向图标栏 + 若干卡片分组
//
// 后续 ContextMenuHost 负责从 shell 的 IContextMenu 填充此模型,
// MenuWindow 只负责渲染 + 交互, 二者互不依赖。

#pragma once

#include <string>
#include <vector>

namespace wsm {

struct MenuItem {
    std::wstring text;         // 显示文字
    std::wstring icon;         // Segoe MDL2 字形 (可空)
    std::wstring verb;         // canonical verb (InvokeCommand 用, 可空)
    int         commandId = 0; // QueryContextMenu 返回的命令 ID (InvokeCommand 用)
    bool        separator = false; // 分隔线
    bool        enabled = true;    // 是否可用 (禁用置灰)
    bool        hasSubmenu = false;
    std::vector<MenuItem> submenu;
};

struct MenuGroup {
    std::vector<MenuItem> items;
};

struct MenuModel {
    std::vector<MenuItem>  topBar; // 顶部横向图标按钮 (复制/粘贴/重命名/删除)
    std::vector<MenuGroup> groups; // 下方卡片分组

    bool empty() const {
        return topBar.empty() && groups.empty();
    }
};

} // namespace wsm
