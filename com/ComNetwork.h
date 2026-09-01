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
// ComNetwork.h -- CP2PNetworkCom, the coclass P2PNetwork.
//
// The one creatable object: CoCreate it, call CreateHub, release it last.
// It owns the p2pf::IP2PNetwork reference (which is itself a process-wide
// refcounted singleton inside the facade) and a strong reference to every hub
// it created, mirroring the facade's own rule that the network owns its hubs.
#pragma once

#include "TargetCom_h.h"
#include "resource.h"

class CP2PHubCom;

class ATL_NO_VTABLE CP2PNetworkCom
    : public ATL::CComObjectRootEx<ATL::CComMultiThreadModel>
    , public ATL::CComCoClass<CP2PNetworkCom, &CLSID_P2PNetwork>
    , public ATL::IDispatchImpl<IP2PNetworkCom, &IID_IP2PNetworkCom, &LIBID_TargetComLib, 1, 0>
    , public ATL::ISupportErrorInfoImpl<&IID_IP2PNetworkCom>
{
  public:
    CP2PNetworkCom ( );

    DECLARE_REGISTRY_RESOURCEID(IDR_P2PNETWORK)
    DECLARE_NOT_AGGREGATABLE(CP2PNetworkCom)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(CP2PNetworkCom)
        COM_INTERFACE_ENTRY(IP2PNetworkCom)
        COM_INTERFACE_ENTRY(IDispatch)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
    END_COM_MAP()

    HRESULT FinalConstruct ( );
    void    FinalRelease   ( );

    // --- IP2PNetworkCom ---------------------------------------------------
    STDMETHOD(CreateHub)          ( BSTR address, IP2PHubCom **ppHub );
    STDMETHOD(get_VersionString)  ( BSTR *pVal );
    STDMETHOD(get_MaxPayload)     ( LONG *pVal );
    STDMETHOD(Link)               ( BSTR listenerAddr, BSTR dialerAddr, BSTR endpoint );
    STDMETHOD(SetEndpoint)        ( BSTR address, BSTR endpoint );
    STDMETHOD(SetEndpointMap)     ( BSTR text );
    STDMETHOD(EndpointFor)        ( BSTR address, BSTR *pVal );
    // facade ABI 8. Not tracked in m_hubs and not closed at shutdown: a
    // message holds no hub, no kernel object and no thread, so there is
    // nothing here to own -- the client releases it like any other object.
    STDMETHOD(CreateMessage)      ( IP2PMessageCom **ppMessage );

    // Called by CP2PHubCom::Close: drops the network's reference to that hub.
    void ForgetHub ( CP2PHubCom *pHub );

  private:
    p2pf::IP2PNetwork           *m_pNet;
    ATL::CComAutoCriticalSection m_csHubs;
    std::vector<IUnknown*>       m_hubs;      // one counted reference each
};

OBJECT_ENTRY_AUTO(__uuidof(P2PNetwork), CP2PNetworkCom)
