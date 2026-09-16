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
// stdafx.h : precompiled header for TargetCom (ATL in-proc COM server)
//
// This DLL is pure ATL -- NO MFC, no Targetcore, no WinSock.  It sees the
// wrapped kernel only through the facade's public header, exactly like any
// other client, which is the point: if TargetCom compiles, the facade is not
// leaking its internals.
#pragma once

#include "Targetver.h"

#define WIN32_LEAN_AND_MEAN
#define STRICT
#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
// Objects here are explicitly CComMultiThreadModel and register
// ThreadingModel=Both, so no _ATL_*_THREADED define is needed.

#include <windows.h>
#include <objbase.h>
#include <process.h>

#include <atlbase.h>
#include <atlcom.h>
#include <atlctl.h>

#include <deque>
#include <map>
#include <vector>

// The facade's public header -- the ONLY view TargetCom has of Targetcore.
#include "TargetFacade.h"
