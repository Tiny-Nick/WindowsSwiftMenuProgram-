// dllmain.h : v4 重构 - SSODL 服务对象声明
#pragma once

#include <atlbase.h>
#include <atlcom.h>

// SSODL 服务 CLSID (explorer 启动时 CoCreateInstance 此对象)
// {7E5D2C9A-3F4B-4A8E-9C61-B2D8E4F6A913}
extern __declspec(selectany) const CLSID CLSID_SwiftMenuService = {
    0x7e5d2c9a, 0x3f4b, 0x4a8e, {0x9c, 0x61, 0xb2, 0xd8, 0xe4, 0xf6, 0xa9, 0x13}
};

class CSwiftMenuModule : public ATL::CAtlDllModuleT<CSwiftMenuModule> {
};

extern CSwiftMenuModule _AtlModule;

// SSODL 空服务对象: 仅要求 CoCreateInstance 成功, 使 explorer 加载本 DLL
class CServiceObject
    : public ATL::CComObjectRootEx<ATL::CComSingleThreadModel>,
      public ATL::CComCoClass<CServiceObject, &CLSID_SwiftMenuService>,
      public IUnknown {
public:
    DECLARE_NO_REGISTRY()

    BEGIN_COM_MAP(CServiceObject)
    END_COM_MAP()
};
