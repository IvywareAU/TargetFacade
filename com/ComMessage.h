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
// ComMessage.h -- CP2PMessageCom, the coclass P2PMessage.
//
// A thin wrapper over p2pf::IP2PMessage, and thin is the whole of it: the flat
// object is already a VALUE with no hub, no thread rule and no kernel handle,
// so there is nothing here to marshal carefully, nothing to queue, and no
// apartment problem. Compare CP2PHubCom, which needs a dispatch thread, a GIT
// and an event queue precisely because a hub is none of those things.
//
// Noncreatable: CP2PNetworkCom::CreateMessage makes one. There is no CLSID
// registration because there is nothing for CoCreateInstance to do -- an empty
// message is not a service.
#pragma once

#include "TargetCom_h.h"

class ATL_NO_VTABLE CP2PMessageCom
    : public ATL::CComObjectRootEx<ATL::CComMultiThreadModel>
    , public ATL::IDispatchImpl<IP2PMessageCom, &IID_IP2PMessageCom, &LIBID_TargetComLib, 1, 0>
    , public ATL::ISupportErrorInfoImpl<&IID_IP2PMessageCom>
{
  public:
    CP2PMessageCom ( );

    BEGIN_COM_MAP(CP2PMessageCom)
        COM_INTERFACE_ENTRY(IP2PMessageCom)
        COM_INTERFACE_ENTRY(IDispatch)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
    END_COM_MAP()

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    void FinalRelease ( );

    // Called by CP2PNetworkCom right after construction.
    HRESULT Init ( p2pf::IP2PNetwork *pNet );

    // What CP2PHubCom::SendMsg needs: the flat object to pass through.
    p2pf::IP2PMessage* Flat ( ) const { return m_pMsg; }

    // --- IP2PMessageCom ---------------------------------------------------
    STDMETHOD(SetPayload)     ( VARIANT payload );
    STDMETHOD(SetField)       ( BSTR name, VARIANT value );
    STDMETHOD(SetFieldText)   ( BSTR name, BSTR text );
    STDMETHOD(RemoveField)    ( BSTR name, VARIANT_BOOL *pRemoved );
    STDMETHOD(get_FieldCount) ( LONG *pVal );
    STDMETHOD(FieldName)      ( LONG index, BSTR *pVal );
    STDMETHOD(GetField)       ( BSTR name, VARIANT *pVal );
    STDMETHOD(Clear)          ( );

  private:
    p2pf::IP2PMessage *m_pMsg;
};
