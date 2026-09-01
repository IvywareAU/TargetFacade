// Copyright © 2026 Khrustal & Mann
//              MELBOURNE, VICTORIA, AUSTRALIA, 3000
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// ComModule.cpp -- the ATL module object and the in-proc server entry points.
#include "stdafx.h"

#include "TargetCom_h.h"
#include "TargetCom_i.c"        // the one definition of every GUID declared above

#include "ComNetwork.h"
#include "ComHub.h"

class CTargetComModule : public ATL::CAtlDllModuleT<CTargetComModule>
{
  public:
    DECLARE_LIBID(LIBID_TargetComLib)
};

CTargetComModule _AtlModule;

extern "C" BOOL WINAPI DllMain ( HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved )
{
    hInstance;
    return _AtlModule.DllMain ( dwReason, lpReserved );
}

// Nothing may unload us while a hub's dispatch thread is still running; each
// live hub holds a reference, and the module's object count follows those.
STDAPI DllCanUnloadNow ( void )
{
    return _AtlModule.DllCanUnloadNow();
}

STDAPI DllGetClassObject ( REFCLSID rclsid, REFIID riid, LPVOID *ppv )
{
    return _AtlModule.DllGetClassObject ( rclsid, riid, ppv );
}

STDAPI DllRegisterServer ( void )
{
    return _AtlModule.DllRegisterServer();      // TRUE: also register the typelib
}

STDAPI DllUnregisterServer ( void )
{
    return _AtlModule.DllUnregisterServer();
}

// regsvr32 /i:"user" -- per-user registration, no elevation needed.
STDAPI DllInstall ( BOOL bInstall, LPCWSTR pszCmdLine )
{
    HRESULT hr = E_FAIL;
    static const wchar_t wszUserSwitch[] = L"user";

    if ( pszCmdLine != NULL && ::_wcsnicmp ( pszCmdLine, wszUserSwitch, _countof(wszUserSwitch) - 1 ) == 0 )
        ATL::AtlSetPerUserRegistration ( true );

    if ( bInstall )
    {
        hr = DllRegisterServer();
        if ( FAILED(hr) )
            DllUnregisterServer();
    }
    else
    {
        hr = DllUnregisterServer();
    }

    return hr;
}
