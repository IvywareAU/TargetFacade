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
// stdafx.h : standard precompiled header for TargetFacade (regular MFC DLL)
//
// Targetcore is MFC-based, so the facade DLL uses MFC dynamically -- but the
// PUBLIC header (include\TargetFacade.h) stays MFC-free: everything here is
// internal to the DLL.
#pragma once

#include "Targetver.h"

#pragma warning(disable:4251)

#define WIN32_LEAN_AND_MEAN

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS
#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#include <afx.h>
#include <afxwin.h>
#include <afxext.h>
#include <afxmt.h>
#include <afxtempl.h>
#include <comutil.h>

#include <WinSock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>

#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include <map>
#include <list>
#include <string>
#include <utility>

// Targetcore -- the wrapped kernel (internal use only; never re-exported)
#include "P2Pwin32.h"
#include "P2PeerHub.h"
#include "P2PeerConWsa.h"
#include "P2PeerConPipe.h"
#include "P2PeerConDmx.h"
#include "P2PeerCon232.h"
#include "P2Peerio.h"
#include "P2PeerioDmx.h"   // Dmx is the one transport with its own protocol
#include "P2PeerMsg.h"
#include "Msgexception.h"
