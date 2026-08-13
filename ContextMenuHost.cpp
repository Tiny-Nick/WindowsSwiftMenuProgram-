// ContextMenuHost.cpp - 真实 shell 右键菜单数据源
//
// 流程:
//   Build(x, y):
//     1. 判定点击目标: 桌面(Progman/WorkerW) 或 资源管理器(CabinetWClass)
//     2. 桌面背景 -> SHGetDesktopFolder + CreateViewObject(IContextMenu)
//        资源管理器 -> IShellWindows 定位浏览器 -> IShellView -> IFolderView
//                     -> 选中项 PIDL -> GetUIObjectOf(IContextMenu)
//     3. QueryContextMenu 到隐藏宿主窗口, 枚举项(文字/verb/命令/子菜单)
//     4. 按 verb 把 复制/粘贴/重命名/删除 归入顶栏, 其余按分隔线分组为卡片
//   Invoke(item): IContextMenu::InvokeCommand(MAKEINTRESOURCEA(commandId))
//
// 已知限制 (后续迭代):
//   - 依赖"选中项"(SVGIO_SELECTION); 右键点击未选中项时可能拿到旧选择
//   - 未实现 IContextMenu2/3 自绘转发, 第三方自绘项显示为纯文字
//   - 桌面图标被当作桌面背景处理

#include "pch.h"
#include "ContextMenuHost.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>

#include <algorithm>
#include <vector>

#include "Log.h"

#pragma comment(lib, "uuid.lib")

namespace wsm {
namespace {

IContextMenu* g_ctxMenu = nullptr; // 当前上下文菜单 (Build 持有, Invoke 使用)
HWND          g_parent = nullptr;  // QueryContextMenu/InvokeCommand 宿主窗口

void ReleaseMenu() {
    if (g_ctxMenu) {
        g_ctxMenu->Release();
        g_ctxMenu = nullptr;
    }
}

// 取项的 canonical verb (GCS_VERBW)
std::wstring GetVerb(IContextMenu* cm, int commandId) {
    if (!cm || commandId <= 0)
        return L"";
    wchar_t buf[128] = {};
    if (SUCCEEDED(cm->GetCommandString(static_cast<UINT_PTR>(commandId - 1), GCS_VERBW,
                                       nullptr, reinterpret_cast<LPSTR>(buf), 128)))
        return buf;
    return L"";
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// 递归枚举一个 HMENU 为 MenuItem 列表
void EnumerateMenu(HMENU menu, std::vector<MenuItem>& out) {
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING | MIIM_SUBMENU | MIIM_STATE;
        wchar_t buf[512] = {};
        mii.dwTypeData = buf;
        mii.cch = 512;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii))
            continue;

        if (mii.fType & MFT_SEPARATOR) {
            MenuItem s;
            s.separator = true;
            out.push_back(std::move(s));
            continue;
        }
        if (mii.fType & MFT_BITMAP)
            continue; // 位图项(罕见), 跳过

        MenuItem it;
        it.text = (mii.dwTypeData && mii.dwTypeData[0]) ? mii.dwTypeData : L"(自定义项)";
        it.commandId = static_cast<int>(mii.wID);
        it.enabled = !(mii.fState & (MFS_DISABLED | MFS_GRAYED));
        it.verb = GetVerb(g_ctxMenu, it.commandId);

        if (mii.hSubMenu) {
            it.hasSubmenu = true;
            EnumerateMenu(mii.hSubMenu, it.submenu);
        }
        out.push_back(std::move(it));
    }
}

// 桌面背景菜单
bool GetDesktopMenu(HWND parent, IContextMenu** out) {
    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop)
        return false;
    HRESULT hr = desktop->CreateViewObject(parent, IID_IContextMenu,
                                           reinterpret_cast<void**>(out));
    desktop->Release();
    return SUCCEEDED(hr) && *out;
}

// 资源管理器选中项菜单
bool GetExplorerMenu(HWND root, HWND parent, IContextMenu** out) {
    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, reinterpret_cast<void**>(&psw))) || !psw)
        return false;

    bool ok = false;
    long count = 0;
    psw->get_Count(&count);

    for (long i = 0; i < count && !ok; ++i) {
        VARIANT v;
        VariantInit(&v);
        V_VT(&v) = VT_I4;
        V_I4(&v) = i;
        IDispatch* disp = nullptr;
        if (FAILED(psw->Item(v, &disp)) || !disp)
            continue;

        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_IServiceProvider,
                                           reinterpret_cast<void**>(&sp))) && sp) {
            IShellBrowser* sb = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                           reinterpret_cast<void**>(&sb))) && sb) {
                HWND bw = nullptr;
                if (SUCCEEDED(sb->GetWindow(&bw)) && bw == root) {
                    IShellView* sv = nullptr;
                    if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
                        IFolderView* fv = nullptr;
                        if (SUCCEEDED(sv->QueryInterface(IID_IFolderView,
                                                         reinterpret_cast<void**>(&fv))) && fv) {
                            IShellFolder* folder = nullptr;
                            if (SUCCEEDED(fv->GetFolder(IID_IShellFolder,
                                                        reinterpret_cast<void**>(&folder))) && folder) {
                                IEnumIDList* penum = nullptr;
                                if (SUCCEEDED(fv->Items(SVGIO_SELECTION,
                                                        IID_PPV_ARGS(&penum))) && penum) {
                                    std::vector<LPITEMIDLIST> pidls;
                                    LPITEMIDLIST pidl = nullptr;
                                    while (penum->Next(1, &pidl, nullptr) == S_OK && pidl)
                                        pidls.push_back(pidl);
                                    penum->Release();
                                    if (!pidls.empty()) {
                                        HRESULT hr = folder->GetUIObjectOf(
                                            parent, static_cast<UINT>(pidls.size()),
                                            (LPCITEMIDLIST*)pidls.data(),
                                            IID_IContextMenu, nullptr,
                                            reinterpret_cast<void**>(out));
                                        ok = SUCCEEDED(hr) && *out;
                                    }
                                    for (auto p : pidls)
                                        CoTaskMemFree(p);
                                }
                                folder->Release();
                            }
                            fv->Release();
                        }
                        sv->Release();
                    }
                }
                sb->Release();
            }
            sp->Release();
        }
        disp->Release();
    }

    psw->Release();
    return ok;
}

} // namespace

bool ContextMenuHost::Build(int screenX, int screenY, HWND parentHwnd, MenuModel& out) {
    ReleaseMenu();

    POINT pt{ screenX, screenY };
    HWND hwnd = WindowFromPoint(pt);
    HWND root = GetAncestor(hwnd, GA_ROOT);

    wchar_t cls[64] = {};
    if (root)
        GetClassNameW(root, cls, 64);
    const bool isDesktop = (root == nullptr || wcscmp(cls, L"Progman") == 0 ||
                            wcscmp(cls, L"WorkerW") == 0);

    IContextMenu* cm = nullptr;
    if (isDesktop) {
        if (!GetDesktopMenu(parentHwnd, &cm)) {
            WSM_LOG(L"[Shell] 桌面菜单获取失败");
            return false;
        }
    } else {
        if (!GetExplorerMenu(root, parentHwnd, &cm)) {
            WSM_LOG(L"[Shell] 资源管理器菜单获取失败 (root=%ls)", cls);
            return false;
        }
    }

    // QueryContextMenu
    HMENU hmenu = CreatePopupMenu();
    HRESULT hr = cm->QueryContextMenu(hmenu, 0, 1, 0x7FFF,
                                      CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME);
    if (FAILED(hr) || hr == 0) {
        DestroyMenu(hmenu);
        cm->Release();
        WSM_LOG(L"[Shell] QueryContextMenu 失败 hr=0x%08X", hr);
        return false;
    }

    g_ctxMenu = cm;
    g_parent = parentHwnd;

    // 枚举
    std::vector<MenuItem> flat;
    EnumerateMenu(hmenu, flat);
    DestroyMenu(hmenu);

    if (flat.empty()) {
        WSM_LOG(L"[Shell] 菜单项为空");
        return true; // 空菜单, 不弹
    }

    // 分类: 顶栏 (复制/粘贴/重命名/删除) + 列表卡片
    struct TopDef { const wchar_t* verb; const wchar_t* label; const wchar_t* icon; };
    static const TopDef kTop[] = {
        { L"copy",   L"复制",   L"\uE8C8" },
        { L"paste",  L"粘贴",   L"\uE77F" },
        { L"rename", L"重命名", L"\uE8AC" },
        { L"delete", L"删除",   L"\uE74D" },
    };

    std::vector<MenuItem> body;
    body.reserve(flat.size());

    for (auto& it : flat) {
        if (it.separator) {
            body.push_back(it);
            continue;
        }
        bool isTop = false;
        for (const auto& t : kTop) {
            if (Lower(it.verb) == t.verb) {
                MenuItem tb = it;
                tb.text = t.label;
                tb.icon = t.icon;
                out.topBar.push_back(std::move(tb));
                isTop = true;
                break;
            }
        }
        if (!isTop)
            body.push_back(std::move(it));
    }

    // 按分隔线分组
    MenuGroup cur;
    for (auto& it : body) {
        if (it.separator) {
            if (!cur.items.empty())
                out.groups.push_back(std::move(cur));
            cur = MenuGroup();
        } else {
            cur.items.push_back(std::move(it));
        }
    }
    if (!cur.items.empty())
        out.groups.push_back(std::move(cur));

    WSM_LOG(L"[Shell] 菜单构建完成: 顶栏=%zu 卡片=%zu", out.topBar.size(), out.groups.size());
    return true;
}

void ContextMenuHost::Invoke(const MenuItem& item) {
    if (!g_ctxMenu || item.commandId <= 0)
        return;

    CMINVOKECOMMANDINFO info = {};
    info.cbSize = sizeof(info);
    info.hwnd = g_parent;
    info.lpVerb = MAKEINTRESOURCEA(item.commandId);
    info.nShow = SW_SHOWNORMAL;

    HRESULT hr = g_ctxMenu->InvokeCommand(&info);
    WSM_LOG(L"[Shell] InvokeCommand cmd=%d hr=0x%08X", item.commandId, hr);

    ReleaseMenu();
}

} // namespace wsm
