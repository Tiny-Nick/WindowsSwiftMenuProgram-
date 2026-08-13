// WindowsSwiftMenu.cpp : DLL 标准导出 (ATL)
#include "pch.h"
#include "framework.h"
#include "dllmain.h"

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return _AtlModule.DllCanUnloadNow();
}

STDAPI DllRegisterServer()
{
    return _AtlModule.DllRegisterServer();
}

STDAPI DllUnregisterServer()
{
    return _AtlModule.DllUnregisterServer();
}
