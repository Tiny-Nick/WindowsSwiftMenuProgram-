// MenuWindow.cpp - 自绘右键菜单窗口
//
// 渲染管线: D3D11 -> DComp -> IDCompositionSurface -> Direct2D/DirectWrite
//   - 窗口 WS_POPUP + WS_EX_NOREDIRECTIONBITMAP, 内容完全由 DComp 合成
//   - DWM 亚克力背景 (DWMSBT_TRANSIENTWINDOW) + 系统圆角
//   - 内容用 Direct2D 绘制 (半透明卡片 + DirectWrite 文字 + MDL2 字形图标)
//   - 透明区域露出亚克力, 无 XAML, 毫秒级弹出
//
// 动画: 淡入 + 轻微放大 (DComp visual 的 opacity/scale, GPU 合成, 不 resize)
//
// 布局: 顶部横向图标栏 (复制/粘贴/重命名/删除) + 下方若干"卡片"(圆角矩形分组),
//       含子菜单的项右侧显示箭头, 悬停展开二级菜单 flyout。
//
// 线程: 独立菜单线程(STA), 窗口 + 消息循环都在该线程; 钩子线程只 PostMessage。

#include "pch.h"
#include "MenuWindow.h"
#include "ContextMenuHost.h"

#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "Log.h"

#undef min
#undef max

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace wsm {
namespace {

constexpr wchar_t kMenuClass[] = L"SwiftMenuPopupWnd";

// ---- 尺寸 ----
constexpr int kMenuWidth    = 320;  // 菜单宽度
constexpr int kPadding      = 4;    // 窗口外圈留白
constexpr int kCardRadius   = 8;    // 卡片圆角
constexpr int kGap          = 4;    // 卡片间距
constexpr int kTopBarHeight = 52;   // 顶部图标栏高
constexpr int kItemHeight   = 36;   // 列表项高

// ---- 字号 ----
constexpr float kItemFontSize    = 13.0f;
constexpr float kTopFontSize     = 12.0f;
constexpr float kIconFontSize    = 16.0f;
constexpr float kChevronFontSize = 12.0f;

// ---- 动画 ----
constexpr double   kAnimDuration   = 0.12; // 秒
constexpr UINT_PTR kAnimTimerId    = 10;
constexpr UINT_PTR kSubmenuTimerId = 11;
constexpr UINT     kSubmenuDelayMs = 130;

constexpr wchar_t kChevronGlyph[] = L"\uE76C"; // ChevronRight

// ---- 主题 ----
struct Theme {
    D2D1_COLOR_F text;
    D2D1_COLOR_F textDisabled;
    D2D1_COLOR_F card;    // 卡片底色(半透明)
    D2D1_COLOR_F hover;
    D2D1_COLOR_F sep;
};
Theme g_theme;

// 构造预乘 alpha 颜色 (D2D 工作在预乘空间)
inline D2D1_COLOR_F Rgba(float r, float g, float b, float a) {
    return D2D1::ColorF(r * a, g * a, b * a, a);
}

void LoadTheme() {
    bool dark = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD v = 1, sz = sizeof(v);
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&v), &sz);
        RegCloseKey(key);
        dark = (v == 0);
    }
    if (dark) {
        g_theme.text         = Rgba(0.95f, 0.95f, 0.95f, 1.0f);
        g_theme.textDisabled = Rgba(0.55f, 0.55f, 0.55f, 1.0f);
        g_theme.card         = Rgba(0.13f, 0.13f, 0.13f, 0.72f);
        g_theme.hover        = Rgba(1.0f, 1.0f, 1.0f, 0.06f);
        g_theme.sep          = Rgba(1.0f, 1.0f, 1.0f, 0.08f);
    } else {
        g_theme.text         = Rgba(0.10f, 0.10f, 0.10f, 1.0f);
        g_theme.textDisabled = Rgba(0.55f, 0.55f, 0.55f, 1.0f);
        g_theme.card         = Rgba(1.0f, 1.0f, 1.0f, 0.72f);
        g_theme.hover        = Rgba(0.0f, 0.0f, 0.0f, 0.05f);
        g_theme.sep          = Rgba(0.0f, 0.0f, 0.0f, 0.08f);
    }
}

// ---- 全局图形资源 (菜单线程独占) ----
ID3D11Device*        g_d3d      = nullptr;
IDCompositionDevice* g_dcomp    = nullptr;
ID2D1Device*         g_d2dDev   = nullptr;
ID2D1DeviceContext*  g_d2dCtx   = nullptr;
ID2D1SolidColorBrush* g_brush   = nullptr;

IDWriteFactory*     g_dwrite   = nullptr;
IDWriteTextFormat*  g_fmtItem  = nullptr; // 列表文字 13px 左对齐/垂直居中
IDWriteTextFormat*  g_fmtTop   = nullptr; // 顶栏标签 12px 居中
IDWriteTextFormat*  g_fmtIcon  = nullptr; // MDL2 图标 16px 居中
IDWriteTextFormat*  g_fmtChev  = nullptr; // 箭头 12px 居中

// ---- 全局状态 ----
HINSTANCE g_hInst = nullptr;
HANDLE    g_thread = nullptr;
volatile LONG g_running = 0;

// ---- 布局 ----
struct GroupLay {
    D2D1_RECT_F card;
    struct Item { D2D1_RECT_F rect; const MenuItem* mi; };
    std::vector<Item> items;
};
struct Layout {
    float width = 0, height = 0;
    std::vector<D2D1_RECT_F> topBtns;
    std::vector<GroupLay> groups;
};

// ---- 单个弹出窗口 ----
struct Popup {
    HWND hwnd = nullptr;
    IDCompositionTarget*   target = nullptr;
    IDCompositionVisual3*  visual = nullptr;
    IDCompositionSurface*  surface = nullptr;
    UINT surfW = 0, surfH = 0;

    bool isSubmenu = false;
    MenuModel model;
    Layout layout;

    int hoverTop = -1, hoverGroup = -1, hoverItem = -1;

    Popup* submenu = nullptr;   // 子菜单 flyout (主菜单持有)
    int submenuGroup = -1, submenuItem = -1;

    bool animating = false;
    ULONGLONG animStart = 0;

    int x = 0, y = 0;
};

Popup* g_main = nullptr; // 主菜单窗口 (线程启动时创建, 常驻)

void SetScale(Popup* p, float s);

double EaseOutCubic(double t) {
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

inline bool PtInRectF(const D2D1_RECT_F& r, POINT pt) {
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

// ---------------------------------------------------------------------------
// 布局计算 (绘制与命中测试共用同一几何, 杜绝错位)
// ---------------------------------------------------------------------------

Layout ComputeLayout(const MenuModel& model) {
    Layout L;
    L.width = static_cast<float>(kMenuWidth);
    float y = static_cast<float>(kPadding);

    if (!model.topBar.empty()) {
        const int n = static_cast<int>(model.topBar.size());
        const float btnW = (L.width - 2 * kPadding) / n;
        for (int i = 0; i < n; ++i) {
            L.topBtns.push_back(D2D1::RectF(kPadding + i * btnW, y,
                                            kPadding + (i + 1) * btnW, y + kTopBarHeight));
        }
        y += kTopBarHeight + kGap;
    }

    for (const auto& g : model.groups) {
        GroupLay gl;
        const float cardY = y;
        const float innerX = kPadding + 4;
        const float innerW = L.width - 2 * kPadding - 8;
        float itemY = cardY + 4;
        for (const auto& it : g.items) {
            if (it.separator) {
                itemY += 5;
                GroupLay::Item s;
                s.rect = D2D1::RectF(innerX, itemY, innerX + innerW, itemY + 1);
                s.mi = &it;
                gl.items.push_back(s);
                itemY += 1 + 5;
                continue;
            }
            GroupLay::Item s;
            s.rect = D2D1::RectF(innerX, itemY, innerX + innerW, itemY + kItemHeight);
            s.mi = &it;
            gl.items.push_back(s);
            itemY += kItemHeight;
        }
        gl.card = D2D1::RectF(kPadding, cardY, L.width - kPadding, itemY + 4);
        L.groups.push_back(gl);
        y = itemY + 4 + kGap;
    }

    L.height = y - kGap + kPadding;
    if (model.empty())
        L.height = 0;
    return L;
}

// ---------------------------------------------------------------------------
// 图形资源初始化 (幂等)
// ---------------------------------------------------------------------------

bool InitGraphics() {
    if (g_dcomp)
        return true;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &g_d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
    if (FAILED(hr)) {
        WSM_LOG(L"[Menu] D3D11 设备创建失败 hr=0x%08X", hr);
        return false;
    }

    IDXGIDevice* dxgiDev = nullptr;
    g_d3d->QueryInterface(IID_PPV_ARGS(&dxgiDev));

    if (FAILED(DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_dcomp)))) {
        WSM_LOG(L"[Menu] DComp 设备创建失败");
        dxgiDev->Release();
        return false;
    }

    ID2D1Factory1* d2dFactory = nullptr;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2dFactory));
    if (SUCCEEDED(hr)) {
        d2dFactory->CreateDevice(dxgiDev, &g_d2dDev);
        d2dFactory->Release();
    }
    dxgiDev->Release();

    if (!g_d2dDev) {
        WSM_LOG(L"[Menu] D2D 设备创建失败");
        return false;
    }
    g_d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2dCtx);
    if (!g_d2dCtx) {
        WSM_LOG(L"[Menu] D2D 上下文创建失败");
        return false;
    }

    g_d2dCtx->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &g_brush);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&g_dwrite));

    if (g_dwrite) {
        auto mk = [](IDWriteTextFormat** out, const wchar_t* family, float size,
                     DWRITE_TEXT_ALIGNMENT ta, DWRITE_PARAGRAPH_ALIGNMENT pa) {
            g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       size, L"en-US", out);
            if (*out) {
                (*out)->SetTextAlignment(ta);
                (*out)->SetParagraphAlignment(pa);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };
        mk(&g_fmtItem, L"Segoe UI", kItemFontSize,
           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        mk(&g_fmtTop, L"Segoe UI", kTopFontSize,
           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        mk(&g_fmtIcon, L"Segoe MDL2 Assets", kIconFontSize,
           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        mk(&g_fmtChev, L"Segoe MDL2 Assets", kChevronFontSize,
           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    LoadTheme();
    WSM_LOG(L"[Menu] 图形管线就绪 (DComp + Direct2D)");
    return true;
}

// ---------------------------------------------------------------------------
// 绘制辅助
// ---------------------------------------------------------------------------

void FillRoundRect(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r,
                   float radius, D2D1_COLOR_F color) {
    g_brush->SetColor(color);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    ctx->FillRoundedRectangle(rr, g_brush);
}

void DrawTextLayout(ID2D1DeviceContext* ctx, const std::wstring& s,
                    IDWriteTextFormat* fmt, D2D1_COLOR_F color,
                    const D2D1_RECT_F& rect) {
    if (s.empty() || !fmt)
        return;
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(g_dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()),
                                          fmt, rect.right - rect.left,
                                          rect.bottom - rect.top, &tl)))
        return;
    g_brush->SetColor(color);
    ctx->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), tl, g_brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
    tl->Release();
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void RenderPopup(Popup* p) {
    if (!p || !p->surface || !g_d2dCtx)
        return;

    ID2D1DeviceContext* ctx = nullptr;
    POINT offset = {};
    HRESULT hr = p->surface->BeginDraw(nullptr, IID_PPV_ARGS(&ctx), &offset);
    if (FAILED(hr) || !ctx) {
        if (ctx) p->surface->EndDraw();
        return;
    }

    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // 透明 -> 露出亚克力
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                    static_cast<float>(offset.y)));

    // 顶栏
    for (size_t i = 0; i < p->layout.topBtns.size(); ++i) {
        const D2D1_RECT_F& r = p->layout.topBtns[i];
        if (static_cast<int>(i) == p->hoverTop)
            FillRoundRect(ctx, r, kCardRadius, g_theme.hover);
        const MenuItem& it = p->model.topBar[i];
        D2D1_RECT_F ir{ r.left, r.top + 4, r.right, r.top + 28 };
        D2D1_RECT_F tr{ r.left, r.top + 26, r.right, r.bottom - 2 };
        DrawTextLayout(ctx, it.icon, g_fmtIcon, g_theme.text, ir);
        DrawTextLayout(ctx, it.text, g_fmtTop, g_theme.text, tr);
    }

    // 卡片
    for (size_t gi = 0; gi < p->layout.groups.size(); ++gi) {
        const GroupLay& gl = p->layout.groups[gi];
        FillRoundRect(ctx, gl.card, kCardRadius, g_theme.card);
        for (size_t ii = 0; ii < gl.items.size(); ++ii) {
            const GroupLay::Item& itl = gl.items[ii];
            const MenuItem& it = *itl.mi;
            if (it.separator) {
                FillRoundRect(ctx, itl.rect, 0, g_theme.sep);
                continue;
            }
            const bool hovered = (static_cast<int>(gi) == p->hoverGroup &&
                                  static_cast<int>(ii) == p->hoverItem);
            if (hovered)
                FillRoundRect(ctx, itl.rect, 6, g_theme.hover);

            const D2D1_COLOR_F tc = it.enabled ? g_theme.text : g_theme.textDisabled;

            if (!it.icon.empty()) {
                D2D1_RECT_F ir{ itl.rect.left + 12, itl.rect.top,
                                itl.rect.left + 32, itl.rect.bottom };
                DrawTextLayout(ctx, it.icon, g_fmtIcon, tc, ir);
            }
            const float textX = itl.rect.left + 40;
            const float rightLimit = itl.rect.right - 12 - (it.hasSubmenu ? 20.0f : 0.0f);
            D2D1_RECT_F tr{ textX, itl.rect.top, rightLimit, itl.rect.bottom };
            DrawTextLayout(ctx, it.text, g_fmtItem, tc, tr);

            if (it.hasSubmenu) {
                D2D1_RECT_F cr{ itl.rect.right - 24, itl.rect.top,
                                itl.rect.right - 4, itl.rect.bottom };
                DrawTextLayout(ctx, kChevronGlyph, g_fmtChev, tc, cr);
            }
        }
    }

    p->surface->EndDraw();
    g_dcomp->Commit();
}

void EnsureSurface(Popup* p) {
    UINT w = static_cast<UINT>(std::ceil(p->layout.width));
    UINT h = static_cast<UINT>(std::ceil(p->layout.height));
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (p->surface && p->surfW == w && p->surfH == h)
        return;
    if (p->surface) {
        p->visual->SetContent(nullptr);
        p->surface->Release();
        p->surface = nullptr;
    }
    HRESULT hr = g_dcomp->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                        DXGI_ALPHA_MODE_PREMULTIPLIED, &p->surface);
    if (SUCCEEDED(hr) && p->surface) {
        p->surfW = w;
        p->surfH = h;
        p->visual->SetContent(p->surface);
    } else {
        WSM_LOG(L"[Menu] CreateSurface 失败 hr=0x%08X", hr);
    }
}

// ---------------------------------------------------------------------------
// 命中测试
// ---------------------------------------------------------------------------

enum class HitKind { None, Top, Item, Separator };
struct Hit {
    HitKind kind = HitKind::None;
    int top = -1, group = -1, item = -1;
    const MenuItem* mi = nullptr;
};

Hit HitTest(const Popup* p, POINT pt) {
    Hit h;
    for (size_t i = 0; i < p->layout.topBtns.size(); ++i) {
        if (PtInRectF(p->layout.topBtns[i], pt)) {
            h.kind = HitKind::Top;
            h.top = static_cast<int>(i);
            h.mi = &p->model.topBar[i];
            return h;
        }
    }
    for (size_t gi = 0; gi < p->layout.groups.size(); ++gi) {
        const GroupLay& gl = p->layout.groups[gi];
        for (size_t ii = 0; ii < gl.items.size(); ++ii) {
            if (PtInRectF(gl.items[ii].rect, pt)) {
                const MenuItem* mi = gl.items[ii].mi;
                if (mi->separator) { h.kind = HitKind::Separator; return h; }
                h.kind = HitKind::Item;
                h.group = static_cast<int>(gi);
                h.item = static_cast<int>(ii);
                h.mi = mi;
                return h;
            }
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// 窗口创建/销毁
// ---------------------------------------------------------------------------

Popup* CreatePopup(const MenuModel& model, bool isSubmenu) {
    Popup* p = new Popup();
    p->isSubmenu = isSubmenu;
    p->model = model;
    p->layout = ComputeLayout(model);
    const int w = static_cast<int>(std::ceil(p->layout.width));
    const int h = static_cast<int>(std::ceil(p->layout.height));

    p->hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kMenuClass, isSubmenu ? L"SwiftMenuSub" : L"SwiftMenu", WS_POPUP,
        0, 0, w, h, nullptr, nullptr, g_hInst, nullptr);
    if (!p->hwnd) {
        WSM_LOG(L"[Menu] 窗口创建失败 err=%lu", GetLastError());
        delete p;
        return nullptr;
    }

    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));

    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(p->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = DWMSBT_TRANSIENTWINDOW; // 亚克力
    DwmSetWindowAttribute(p->hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    g_dcomp->CreateTargetForHwnd(p->hwnd, TRUE, &p->target);
    IDCompositionVisual* v = nullptr;
    g_dcomp->CreateVisual(&v);
    if (v) {
        v->QueryInterface(IID_PPV_ARGS(&p->visual)); // SetOpacity/SetScale 在 Visual3
        v->Release();
    }
    p->target->SetRoot(p->visual);

    EnsureSurface(p);
    return p;
}

void DestroyPopup(Popup* p) {
    if (!p)
        return;
    if (p->hwnd) {
        SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(p->hwnd);
    }
    if (p->surface) p->surface->Release();
    if (p->visual)  p->visual->Release();
    if (p->target)  p->target->Release();
    delete p;
}

// ---------------------------------------------------------------------------
// 子菜单
// ---------------------------------------------------------------------------

void CloseSubmenu(Popup* parent) {
    if (!parent || !parent->submenu)
        return;
    DestroyPopup(parent->submenu);
    parent->submenu = nullptr;
    parent->submenuGroup = -1;
    parent->submenuItem = -1;
}

void OpenSubmenu(Popup* parent, int gi, int ii) {
    if (!parent || gi < 0 || ii < 0)
        return;
    if (gi >= static_cast<int>(parent->layout.groups.size()))
        return;
    const auto& items = parent->layout.groups[gi].items;
    if (ii >= static_cast<int>(items.size()))
        return;
    const MenuItem* mi = items[ii].mi;
    if (!mi || !mi->hasSubmenu || mi->submenu.empty())
        return;

    MenuModel m;
    MenuGroup g;
    g.items = mi->submenu;
    m.groups.push_back(g);

    Popup* sub = CreatePopup(m, true);
    if (!sub)
        return;
    parent->submenu = sub;
    parent->submenuGroup = gi;
    parent->submenuItem = ii;

    const D2D1_RECT_F ir = items[ii].rect;
    int sx = parent->x + static_cast<int>(parent->layout.width) - kPadding;
    int sy = parent->y + static_cast<int>(ir.top);

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int sw = static_cast<int>(sub->layout.width);
    const int sh = static_cast<int>(sub->layout.height);
    if (sx + sw > work.right) sx = parent->x - sw + kPadding;
    if (sy + sh > work.bottom) sy = work.bottom - sh;
    if (sy < work.top) sy = work.top;

    sub->x = sx;
    sub->y = sy;
    SetWindowPos(sub->hwnd, HWND_TOPMOST, sx, sy, sw, sh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    sub->visual->SetOpacity(0.0f);
    sub->animating = true;
    sub->animStart = GetTickCount64();
    SetTimer(sub->hwnd, kAnimTimerId, 16, nullptr);
    RenderPopup(sub);
    g_dcomp->Commit();
}

// ---------------------------------------------------------------------------
// 显示/隐藏/动画
// ---------------------------------------------------------------------------

// 演示模型 (后续由 ContextMenuHost 从 shell 填充)
MenuModel BuildDemoModel() {
    auto item = [](const wchar_t* text, const wchar_t* icon, const wchar_t* verb = L"") {
        MenuItem m;
        m.text = text;
        m.icon = icon;
        m.verb = verb;
        return m;
    };

    MenuModel m;
    m.topBar = {
        item(L"复制",   L"\uE8C8", L"copy"),
        item(L"粘贴",   L"\uE77F", L"paste"),
        item(L"重命名", L"\uE8AC", L"rename"),
        item(L"删除",   L"\uE74D", L"delete"),
    };

    MenuGroup g1;
    g1.items = {
        item(L"打开",       L"\uE8E5", L"open"),
        item(L"打开方式",   L"\uE8A5", L"openas"),
        item(L"使用记事本打开", L"\uE70F", L"notepad"),
    };
    g1.items[1].hasSubmenu = true;
    g1.items[1].submenu = {
        item(L"记事本",  L"\uE70F"),
        item(L"写字板",  L"\uE70F"),
        item(L"选择其他应用", L"\uE8A5"),
    };

    MenuGroup g2;
    g2.items = {
        item(L"发送到",   L"\uE724", L"sendto"),
        item(L"共享",     L"\uE72D", L"share"),
        item(L"复制文件路径", L"\uE8C8", L"copyaspath"),
    };
    g2.items[0].hasSubmenu = true;
    g2.items[0].submenu = {
        item(L"桌面快捷方式", L"\uE8F0"),
        item(L"邮件收件人",   L"\uE715"),
        item(L"压缩文件夹",   L"\uE8B7"),
    };

    MenuGroup g3;
    g3.items = {
        item(L"固定到快速访问", L"\uE71B", L"pintohome"),
        item(L"属性",           L"\uE946", L"properties"),
    };

    m.groups = { g1, g2, g3 };
    return m;
}

void ShowMain(int x, int y) {
    Popup* p = g_main;
    if (!p || !InitGraphics())
        return;
    CloseSubmenu(p);

    p->model = MenuModel();
    MenuModel model;
    if (!ContextMenuHost::Build(x, y, p->hwnd, model))
        model = BuildDemoModel(); // 降级: 演示数据
    p->model = std::move(model);
    p->layout = ComputeLayout(p->model);

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(std::ceil(p->layout.width));
    const int h = static_cast<int>(std::ceil(p->layout.height));
    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    p->x = x;
    p->y = y;

    p->hoverTop = -1;
    p->hoverGroup = -1;
    p->hoverItem = -1;

    EnsureSurface(p);
    SetWindowPos(p->hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    p->visual->SetOpacity(0.0f);
    SetScale(p, 0.96f);
    p->animating = true;
    p->animStart = GetTickCount64();
    SetTimer(p->hwnd, kAnimTimerId, 16, nullptr);
    RenderPopup(p);
}

void HideAll() {
    if (!g_main)
        return;
    CloseSubmenu(g_main);
    KillTimer(g_main->hwnd, kAnimTimerId);
    KillTimer(g_main->hwnd, kSubmenuTimerId);
    if (IsWindowVisible(g_main->hwnd))
        ShowWindow(g_main->hwnd, SW_HIDE);
    g_main->animating = false;
}

// 设置视觉缩放 (Visual3 的 SetTransform 只接受 4x4 矩阵, 以左上角为原点)
void SetScale(Popup* p, float s) {
    if (!p || !p->visual)
        return;
    D2D_MATRIX_4X4_F m = {};
    m._11 = s;
    m._22 = s;
    m._33 = 1.0f;
    m._44 = 1.0f;
    p->visual->SetTransform(m);
}

void TickAnimation(Popup* p) {
    const double elapsed = (static_cast<double>(GetTickCount64()) - p->animStart) / 1000.0;
    const double t = std::min(1.0, elapsed / kAnimDuration);
    const double e = EaseOutCubic(t);
    p->visual->SetOpacity(static_cast<float>(e));
    const float s = 0.96f + 0.04f * static_cast<float>(e);
    SetScale(p, s);
    g_dcomp->Commit();
    if (t >= 1.0) {
        p->animating = false;
        KillTimer(p->hwnd, kAnimTimerId);
        p->visual->SetOpacity(1.0f);
        SetScale(p, 1.0f);
        g_dcomp->Commit();
    }
}

// ---------------------------------------------------------------------------
// 悬停处理
// ---------------------------------------------------------------------------

void HandleHover(Popup* p, POINT pt, bool isMain) {
    Hit h = HitTest(p, pt);
    const int newTop = (h.kind == HitKind::Top) ? h.top : -1;
    const int newGroup = (h.kind == HitKind::Item) ? h.group : -1;
    const int newItem = (h.kind == HitKind::Item) ? h.item : -1;
    const bool changed = (newTop != p->hoverTop || newGroup != p->hoverGroup ||
                          newItem != p->hoverItem);
    if (changed) {
        p->hoverTop = newTop;
        p->hoverGroup = newGroup;
        p->hoverItem = newItem;
        RenderPopup(p);
    }

    if (!isMain)
        return;

    if (h.kind == HitKind::Item && h.mi && h.mi->hasSubmenu) {
        const bool same = (p->submenu && p->submenuGroup == h.group &&
                           p->submenuItem == h.item);
        if (!same) {
            CloseSubmenu(p);
            KillTimer(p->hwnd, kSubmenuTimerId);
            SetTimer(p->hwnd, kSubmenuTimerId, kSubmenuDelayMs, nullptr);
        }
    } else {
        CloseSubmenu(p);
        KillTimer(p->hwnd, kSubmenuTimerId);
    }
}

void InvokeItem(const MenuItem& mi) {
    WSM_LOG(L"[Menu] 执行: %ls (verb=%ls, cmd=%d)", mi.text.c_str(),
            mi.verb.c_str(), mi.commandId);
    ContextMenuHost::Invoke(mi);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Popup* p = reinterpret_cast<Popup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_APP + 2: { // 钩子通知: 在 (x, y) 显示主菜单 (wParam=x, lParam=y, 完整 32 位)
        ShowMain(static_cast<int>(static_cast<LONG_PTR>(wp)),
                 static_cast<int>(static_cast<LONG_PTR>(lp)));
        return 0;
    }
    case WM_APP + 3: // 点击菜单外部 -> 关闭
        HideAll();
        return 0;

    case WM_TIMER:
        if (!p)
            return 0;
        if (wp == kAnimTimerId) {
            TickAnimation(p);
        } else if (wp == kSubmenuTimerId) {
            KillTimer(hwnd, kSubmenuTimerId);
            OpenSubmenu(p, p->hoverGroup, p->hoverItem);
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (!p)
            break;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HandleHover(p, pt, !p->isSubmenu);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!p)
            break;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Hit h = HitTest(p, pt);
        if (h.kind == HitKind::Top || h.kind == HitKind::Item) {
            if (h.mi && h.mi->enabled) {
                if (h.mi->hasSubmenu && !p->isSubmenu)
                    OpenSubmenu(p, h.group, h.item); // 点击含子菜单项: 直接展开
                else {
                    HideAll();      // 先隐藏, 避免遮挡 shell 对话框
                    InvokeItem(*h.mi);
                }
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
        HideAll();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            HideAll();
            return 0;
        }
        break;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI MenuThreadProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kMenuClass;
    RegisterClassW(&wc);

    if (InitGraphics()) {
        MenuModel empty;
        g_main = CreatePopup(empty, false);
        if (g_main)
            WSM_LOG(L"[Menu] 主菜单窗口就绪 hwnd=%p", g_main->hwnd);
    } else {
        WSM_LOG(L"[Menu] 图形初始化失败, 菜单不可用");
    }

    MSG msg;
    while (InterlockedCompareExchange(&g_running, 1, 1) &&
           GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_main) {
        CloseSubmenu(g_main);
        DestroyPopup(g_main);
        g_main = nullptr;
    }

    CoUninitialize();
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void MenuWindow::Start() {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0)
        return;
    g_hInst = GetModuleHandleW(L"WindowsSwiftMenu.dll");
    g_thread = CreateThread(nullptr, 0, MenuThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        WSM_LOG(L"[Menu] 线程创建失败 err=%lu", GetLastError());
        InterlockedExchange(&g_running, 0);
    }
}

void MenuWindow::StopRequest() {
    InterlockedExchange(&g_running, 0);
    if (g_thread)
        PostThreadMessageW(GetThreadId(g_thread), WM_QUIT, 0, 0);
}

HWND MenuWindow::GetHwnd() {
    return g_main ? g_main->hwnd : nullptr;
}

bool MenuWindow::IsVisible() {
    return g_main && IsWindowVisible(g_main->hwnd);
}

void MenuWindow::ShowAt(int x, int y) {
    HWND h = GetHwnd();
    if (h)
        PostMessageW(h, WM_APP + 2,
                     static_cast<WPARAM>(static_cast<LONG_PTR>(x)),
                     static_cast<LPARAM>(static_cast<LONG_PTR>(y)));
}

void MenuWindow::CloseRequest() {
    HWND h = GetHwnd();
    if (h)
        PostMessageW(h, WM_APP + 3, 0, 0);
}

} // namespace wsm
