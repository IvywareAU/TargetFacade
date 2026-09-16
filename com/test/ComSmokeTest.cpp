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
// ComSmokeTest.cpp
//
// End-to-end test of TargetCom, deliberately written as a SINGLE-THREADED
// APARTMENT client -- the case the whole GIT/dispatch-thread design in
// ComHub.cpp exists for.  Events raised on a hub's dispatch thread have to be
// marshalled into this apartment and delivered through this thread's message
// queue, so every wait below pumps messages.  If the marshalling were wrong,
// the event checks would simply never fire.
//
// It includes NO facade header and NO Targetcore header: the only contract it
// knows is the type library, exactly like a VB/.NET/script client.
//
// It is also the one place both call paths exist side by side, which makes it
// the right place to MEASURE the difference: the last block before teardown
// calls Listen through the vtable and again through IDispatch::Invoke, on the
// same hub in the same process, and asserts what each one can see of a success
// HRESULT.  See "C++ automation" there.
//
// Exit code 0 = all checks passed, otherwise the number of failures.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <olectl.h>          // CONNECT_E_*
#include <stdio.h>
#include <vector>

#include "TargetCom_h.h"
#include "TargetCom_i.c"

// The facade HRESULTs the COM layer passes straight through.
static const HRESULT kReservedTopic = MAKE_HRESULT(1, FACILITY_ITF, 0x0205);
static const HRESULT kClosed        = MAKE_HRESULT(1, FACILITY_ITF, 0x0206);
static const HRESULT kConDuplicate  = MAKE_HRESULT(1, FACILITY_ITF, 0x0204);
static const HRESULT kEndpoint      = MAKE_HRESULT(1, FACILITY_ITF, 0x0208);
static const HRESULT kUnresolved    = MAKE_HRESULT(1, FACILITY_ITF, 0x0209);
static const HRESULT kNoHub         = MAKE_HRESULT(1, FACILITY_ITF, 0x020A);
// A SUCCESS code -- severity 0.  Only a vtable caller like this one ever sees
// it; ITypeInfo::Invoke and the CLR marshaller both normalise it to S_OK,
// which is the whole reason the read side (dispids 9-13) exists.
static const HRESULT kUnrelatedLink = MAKE_HRESULT(0, FACILITY_ITF, 0x020C);
static const HRESULT kSecurity      = MAKE_HRESULT(1, FACILITY_ITF, 0x0218);
// facade ABI 6 -- compared NUMERICALLY here too, so these are appended only
static const HRESULT kNoPeer        = MAKE_HRESULT(1, FACILITY_ITF, 0x020D);
static const HRESULT kTimeout       = MAKE_HRESULT(1, FACILITY_ITF, 0x020E);
static const HRESULT kOption        = MAKE_HRESULT(1, FACILITY_ITF, 0x0210);
// facade ABI 7
static const HRESULT kNoMessage     = MAKE_HRESULT(1, FACILITY_ITF, 0x0212);
// facade ABI 8
static const HRESULT kNoField       = MAKE_HRESULT(1, FACILITY_ITF, 0x0213);

static int g_checks = 0;
static int g_fails  = 0;

static void Check ( bool bOk, const char *szWhat )
{
    ++g_checks;
    if ( !bOk ) { ++g_fails; printf ( "  FAIL  %s\n", szWhat ); }
    else        {            printf ( "  ok    %s\n", szWhat ); }
}

// ---------------------------------------------------------------------------
// The event sink: a hand-written IDispatch, i.e. the lowest-common-denominator
// client.  Invoke() arrives on THIS (apartment) thread inside the pump below,
// so the captured state needs no locking.
// ---------------------------------------------------------------------------
class CSink : public IDispatch
{
  public:
    CSink ( ) : m_lRef ( 1 ), cPeerUp ( 0 ), cPeerDown ( 0 ), cMsg ( 0 ), cError ( 0 )
              , bstrSource ( NULL ), bstrTopic ( NULL ), bstrPeer ( NULL )
              , bBroadcast ( VARIANT_FALSE )
              , cEvent ( 0 ), cTimer ( 0 ), lEventCode ( 0 ), cEventUnrelated ( 0 )
              , bstrEventPeer ( NULL ), lTimerId ( 0 ), lTimerKey ( 0 )
              , pHubForMsg ( NULL ), bstrMsgDest ( NULL )
              , lMsgTag ( 0 ), lMsgPriority ( 0 ), lMsgFlags ( 0 )
              , hrMsgDest ( E_FAIL ), hrMsgTag ( E_FAIL ), hrMsgOutside ( S_OK )
              , lMsgFieldCount ( -1 ), bstrField0 ( NULL )
              , hrMsgField ( E_FAIL ), hrMsgFieldAbsent ( S_OK )
    {
        ::VariantInit ( &vMsgField );
    }
   ~CSink ( )
    {
        ::SysFreeString ( bstrSource ); ::SysFreeString ( bstrTopic );
        ::SysFreeString ( bstrPeer );   ::SysFreeString ( bstrEventPeer );
        ::SysFreeString ( bstrMsgDest ); ::SysFreeString ( bstrField0 );
        ::VariantClear  ( &vMsgField );
    }

    // --- IUnknown ---------------------------------------------------------
    STDMETHOD(QueryInterface) ( REFIID riid, void **ppv )
    {
        if ( ppv == NULL ) return E_POINTER;
        if ( ::IsEqualIID ( riid, IID_IUnknown ) ||
             ::IsEqualIID ( riid, IID_IDispatch ) ||
             ::IsEqualIID ( riid, DIID__IP2PHubEvents ) )
        {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)  ( ) { return ::InterlockedIncrement ( &m_lRef ); }
    STDMETHOD_(ULONG, Release) ( )
    {
        LONG l = ::InterlockedDecrement ( &m_lRef );
        if ( l == 0 ) delete this;
        return l;
    }

    // --- IDispatch (late-bound only; the hub calls Invoke by DISPID) -------
    STDMETHOD(GetTypeInfoCount) ( UINT *pctinfo ) { if (pctinfo) *pctinfo = 0; return S_OK; }
    STDMETHOD(GetTypeInfo)      ( UINT, LCID, ITypeInfo** ) { return E_NOTIMPL; }
    STDMETHOD(GetIDsOfNames)    ( REFIID, LPOLESTR*, UINT, LCID, DISPID* ) { return E_NOTIMPL; }

    STDMETHOD(Invoke) ( DISPID dispid, REFIID, LCID, WORD, DISPPARAMS *pdp
                      , VARIANT*, EXCEPINFO*, UINT* )
    {
        if ( pdp == NULL ) return E_POINTER;

        switch ( dispid )
        {
          case 1:   // OnMessage(source, topic, payload, broadcast) -- reversed
            if ( pdp->cArgs != 4 ) return DISP_E_BADPARAMCOUNT;
            ::SysFreeString ( bstrSource );
            ::SysFreeString ( bstrTopic );
            bstrSource = ::SysAllocString ( pdp->rgvarg[3].bstrVal );
            bstrTopic  = ::SysAllocString ( pdp->rgvarg[2].bstrVal );
            bBroadcast = pdp->rgvarg[0].boolVal;
            CopyPayload ( pdp->rgvarg[1] );
            // The message model (facade ABI 7), read from INSIDE the handler
            // -- which is the only place these answer, and is here a
            // different apartment from the thread that raised the event.
            // Reading them here is the test: if the causality gate were a
            // thread compare, every one of these would be p2pfNoMessage.
            if ( pHubForMsg != NULL )
            {
                ::SysFreeString ( bstrMsgDest ); bstrMsgDest = NULL;
                hrMsgDest = pHubForMsg->get_MsgDest     ( &bstrMsgDest );
                hrMsgTag  = pHubForMsg->get_MsgTag      ( &lMsgTag );
                                        pHubForMsg->get_MsgPriority ( &lMsgPriority );
                                        pHubForMsg->get_MsgFlags    ( &lMsgFlags );
                // Named fields (facade ABI 8), read from the same place and
                // under the same gate.
                lMsgFieldCount = 0;
                ::SysFreeString ( bstrField0 ); bstrField0 = NULL;
                pHubForMsg->get_MsgFieldCount ( &lMsgFieldCount );
                if ( lMsgFieldCount > 0 )
                    pHubForMsg->MsgFieldName ( 0, &bstrField0 );
                ::VariantClear ( &vMsgField );
                BSTR bsWho = ::SysAllocString ( L"who" );
                hrMsgField = pHubForMsg->MsgField ( bsWho, &vMsgField );
                ::SysFreeString ( bsWho );
                BSTR bsNope = ::SysAllocString ( L"nope" );
                VARIANT vJunk; ::VariantInit ( &vJunk );
                hrMsgFieldAbsent = pHubForMsg->MsgField ( bsNope, &vJunk );
                ::VariantClear ( &vJunk );
                ::SysFreeString ( bsNope );
            }
            ++cMsg;
            break;

          case 2:   // OnPeerUp(peer)
            // The same properties from a callback that is NOT a message:
            // same object, same apartment, same causality chain -- and no
            // message, which is the distinction the gate has to make.
            if ( pHubForMsg != NULL )
            {
                LONG lJunk = 0;
                hrMsgOutside = pHubForMsg->get_MsgTag ( &lJunk );
            }
            ::SysFreeString ( bstrPeer );
            bstrPeer = ::SysAllocString ( pdp->rgvarg[0].bstrVal );
            ++cPeerUp;
            break;

          case 3:   // OnPeerDown(peer)
            ++cPeerDown;
            break;

          case 4:   // OnError(what)
            ++cError;
            break;

          case 5:   // OnEvent(code, peer, what) -- reversed, facade ABI 6
            if ( pdp->cArgs != 3 ) return DISP_E_BADPARAMCOUNT;
            lEventCode = pdp->rgvarg[2].lVal;
            // A sink sees more than one KIND of event now that a bounced
            // message reports itself (P2PF_EVT_ROUTING_ERROR), so the last
            // code is no longer the one a check is asking about. Remember
            // each code as SEEN rather than only the most recent.
            if ( lEventCode == p2pfEvtUnrelatedLink ) ++cEventUnrelated;
            ::SysFreeString ( bstrEventPeer );
            bstrEventPeer = ::SysAllocString ( pdp->rgvarg[1].bstrVal );
            ++cEvent;
            break;

          case 6:   // OnTimer(timerId, key) -- reversed
            if ( pdp->cArgs != 2 ) return DISP_E_BADPARAMCOUNT;
            lTimerId  = pdp->rgvarg[1].lVal;
            lTimerKey = pdp->rgvarg[0].lVal;
            ++cTimer;
            break;

          default:
            return DISP_E_MEMBERNOTFOUND;
        }
        return S_OK;
    }

    // captured state
    LONG              cPeerUp, cPeerDown, cMsg, cError;
    BSTR              bstrSource, bstrTopic, bstrPeer;
    VARIANT_BOOL      bBroadcast;
    std::vector<BYTE> payload;
    // facade ABI 6
    LONG              cEvent, cTimer, lEventCode, cEventUnrelated;
    BSTR              bstrEventPeer;
    LONG              lTimerId, lTimerKey;
    // facade ABI 7. `pHubForMsg` is a borrowed pointer the test sets before
    // arming anything; the sink never releases it.
    IP2PHubCom       *pHubForMsg;
    BSTR              bstrMsgDest;
    LONG              lMsgTag, lMsgPriority, lMsgFlags;
    HRESULT           hrMsgDest, hrMsgTag, hrMsgOutside;
    // facade ABI 8
    LONG              lMsgFieldCount;
    BSTR              bstrField0;
    VARIANT           vMsgField;
    HRESULT           hrMsgField, hrMsgFieldAbsent;

  private:
    void CopyPayload ( const VARIANT& v )
    {
        payload.clear();
        if ( ( v.vt & VT_ARRAY ) == 0 || v.parray == NULL ) return;

        LONG lo = 0, hi = -1;
        ::SafeArrayGetLBound ( v.parray, 1, &lo );
        ::SafeArrayGetUBound ( v.parray, 1, &hi );
        if ( hi < lo ) return;

        void *pData = NULL;
        if ( SUCCEEDED ( ::SafeArrayAccessData ( v.parray, &pData ) ) )
        {
            const BYTE *p = (const BYTE*)pData;
            payload.assign ( p, p + ( hi - lo + 1 ) );
            ::SafeArrayUnaccessData ( v.parray );
        }
    }

    LONG m_lRef;
};

// ---------------------------------------------------------------------------
// An STA must pump, or a marshalled event call never arrives.
// ---------------------------------------------------------------------------
static void PumpOnce ( )
{
    MSG msg;
    while ( ::PeekMessage ( &msg, NULL, 0, 0, PM_REMOVE ) )
    {
        ::TranslateMessage ( &msg );
        ::DispatchMessage ( &msg );
    }
}

static bool WaitUntil ( const LONG *pCounter, LONG lTarget, DWORD dwMs )
{
    DWORD dwStart = ::GetTickCount();
    for ( ;; )
    {
        PumpOnce();
        if ( *pCounter >= lTarget ) return true;
        if ( ::GetTickCount() - dwStart > dwMs ) return false;
        ::Sleep ( 10 );
    }
}

// ---------------------------------------------------------------------------
static HRESULT MakeByteArray ( const BYTE *pData, ULONG cb, VARIANT *pv )
{
    ::VariantInit ( pv );
    SAFEARRAY *psa = ::SafeArrayCreateVector ( VT_UI1, 0, cb );
    if ( psa == NULL ) return E_OUTOFMEMORY;

    void *pDst = NULL;
    ::SafeArrayAccessData ( psa, &pDst );
    ::memcpy ( pDst, pData, cb );
    ::SafeArrayUnaccessData ( psa );

    pv->vt = VT_ARRAY | VT_UI1;
    pv->parray = psa;
    return S_OK;
}

int wmain ( )
{
    printf ( "TargetCom smoke test (STA client, late-bound sink)\n" );

    // APARTMENTTHREADED on purpose: the hard marshalling case.
    HRESULT hr = ::CoInitializeEx ( NULL, COINIT_APARTMENTTHREADED );
    if ( FAILED(hr) ) { printf ( "CoInitializeEx failed 0x%08lX\n", hr ); return 99; }

    {
        IP2PNetworkCom *pNet = NULL;
        hr = ::CoCreateInstance ( CLSID_P2PNetwork, NULL, CLSCTX_INPROC_SERVER
                                , IID_IP2PNetworkCom, (void**)&pNet );
        Check ( SUCCEEDED(hr) && pNet != NULL, "CoCreateInstance(P2PNetwork)" );
        if ( FAILED(hr) || pNet == NULL )
        {
            printf ( "cannot continue: 0x%08lX (is the DLL registered?)\n", hr );
            ::CoUninitialize();
            return 99;
        }

        // --- network properties ------------------------------------------
        BSTR bstrVer = NULL;
        hr = pNet->get_VersionString ( &bstrVer );
        Check ( SUCCEEDED(hr) && bstrVer != NULL && ::SysStringLen ( bstrVer ) > 0
              , "VersionString is non-empty" );
        if ( bstrVer ) { wprintf ( L"        %s\n", bstrVer ); ::SysFreeString ( bstrVer ); }

        LONG lMax = 0;
        hr = pNet->get_MaxPayload ( &lMax );
        Check ( SUCCEEDED(hr) && lMax == 24 * 1024, "MaxPayload is 24 KiB" );

        // --- two hubs ------------------------------------------------------
        IP2PHubCom *pS = NULL, *pC = NULL;
        BSTR bstrS = ::SysAllocString ( L"Com.Server" );
        BSTR bstrC = ::SysAllocString ( L"Com.Client" );
        hr = pNet->CreateHub ( bstrS, &pS );
        Check ( SUCCEEDED(hr) && pS != NULL, "CreateHub(Com.Server)" );
        hr = pNet->CreateHub ( bstrC, &pC );
        Check ( SUCCEEDED(hr) && pC != NULL, "CreateHub(Com.Client)" );

        if ( pS == NULL || pC == NULL )
        {
            printf ( "cannot continue without both hubs\n" );
            if ( pS ) pS->Release();
            if ( pC ) pC->Release();
            pNet->Release();
            ::CoUninitialize();
            return 99;
        }

        BSTR bstrAddr = NULL;
        hr = pS->get_Address ( &bstrAddr );
        Check ( SUCCEEDED(hr) && bstrAddr != NULL && ::wcscmp ( bstrAddr, L"Com.Server" ) == 0
              , "Address round-trips as a BSTR" );
        ::SysFreeString ( bstrAddr );

        // --- connection points --------------------------------------------
        CSink *pSinkS = new CSink();
        CSink *pSinkC = new CSink();
        IConnectionPoint *pCpS = NULL, *pCpC = NULL;
        DWORD dwCookieS = 0, dwCookieC = 0;

        IConnectionPointContainer *pCpcS = NULL;
        hr = pS->QueryInterface ( IID_IConnectionPointContainer, (void**)&pCpcS );
        Check ( SUCCEEDED(hr) && pCpcS != NULL, "hub exposes IConnectionPointContainer" );
        if ( pCpcS )
        {
            hr = pCpcS->FindConnectionPoint ( DIID__IP2PHubEvents, &pCpS );
            Check ( SUCCEEDED(hr) && pCpS != NULL, "FindConnectionPoint(_IP2PHubEvents)" );
            IConnectionPoint *pBogus = NULL;
            hr = pCpcS->FindConnectionPoint ( IID_IUnknown, &pBogus );
            Check ( hr == CONNECT_E_NOCONNECTION && pBogus == NULL
                  , "unknown source IID is refused" );
            pCpcS->Release();
        }
        if ( pCpS )
        {
            hr = pCpS->Advise ( pSinkS, &dwCookieS );
            Check ( SUCCEEDED(hr) && dwCookieS != 0, "Advise(server sink)" );
        }

        IConnectionPointContainer *pCpcC = NULL;
        if ( SUCCEEDED ( pC->QueryInterface ( IID_IConnectionPointContainer, (void**)&pCpcC ) ) )
        {
            pCpcC->FindConnectionPoint ( DIID__IP2PHubEvents, &pCpC );
            pCpcC->Release();
        }
        if ( pCpC )
        {
            hr = pCpC->Advise ( pSinkC, &dwCookieC );
            Check ( SUCCEEDED(hr) && dwCookieC != 0, "Advise(client sink)" );
        }

        // Facade ABI 7: let each sink ask its own hub about the message it is
        // being handed. Borrowed pointers, set before anything is armed.
        pSinkS->pHubForMsg = pS;
        pSinkC->pHubForMsg = pC;

        // --- TCP loopback ---------------------------------------------------
        // The transport is a string now, so a script can read it from a
        // config file: one pair of verbs, four schemes, no typed overloads.
        // SUCCEEDED, not == S_OK: "Com.Server" and "Com.Client" are SIBLINGS,
        // so the facade arms the link and reports P2PF_S_UNRELATED_LINK -- a
        // success code -- because a sibling edge can carry direct traffic but
        // can never be routed THROUGH.
        BSTR bstrEpListen = ::SysAllocString ( L"tcp://:7799" );
        BSTR bstrEpDial   = ::SysAllocString ( L"tcp://127.0.0.1:7799" );

        hr = pS->Listen ( bstrC, bstrEpListen );
        Check ( hr == kUnrelatedLink, "Listen(Com.Client, tcp://:7799) -> P2PF_S_UNRELATED_LINK" );

        hr = pC->Connect ( bstrS, bstrEpDial );
        Check ( hr == kUnrelatedLink, "Connect(Com.Server, tcp://127.0.0.1:7799) -> P2PF_S_UNRELATED_LINK" );

        // Port range now lives in the endpoint grammar, inside the facade, and
        // comes back as P2PF_E_ENDPOINT rather than a COM-tier E_INVALIDARG.
        BSTR bstrEpBad0 = ::SysAllocString ( L"tcp://:0" );
        BSTR bstrEpBadHi= ::SysAllocString ( L"tcp://:70000" );
        Check ( pS->Listen ( bstrC, bstrEpBad0 )  == kEndpoint, "port 0 is rejected" );
        Check ( pS->Listen ( bstrC, bstrEpBadHi ) == kEndpoint, "port 70000 is rejected" );
        ::SysFreeString ( bstrEpBad0 ); ::SysFreeString ( bstrEpBadHi );

        // Both sides must report the peer, and the event has to survive being
        // marshalled from the dispatch thread into this apartment.
        Check ( WaitUntil ( &pSinkS->cPeerUp, 1, 15000 ), "server sink got OnPeerUp (marshalled)" );
        Check ( WaitUntil ( &pSinkC->cPeerUp, 1, 15000 ), "client sink got OnPeerUp (marshalled)" );

        VARIANT_BOOL vbUp = VARIANT_FALSE;
        Check ( SUCCEEDED ( pS->IsPeerUp ( bstrC, &vbUp ) ) && vbUp == VARIANT_TRUE
              , "IsPeerUp(server -> client)" );
        Check ( SUCCEEDED ( pC->IsPeerUp ( bstrS, &vbUp ) ) && vbUp == VARIANT_TRUE
              , "IsPeerUp(client -> server)" );

        // --- unicast text ----------------------------------------------------
        BSTR bstrTopic = ::SysAllocString ( L"chat" );
        BSTR bstrHello = ::SysAllocString ( L"hello com" );
        hr = pC->SendText ( bstrS, bstrTopic, bstrHello );
        Check ( SUCCEEDED(hr), "SendText" );
        Check ( WaitUntil ( &pSinkS->cMsg, 1, 10000 ), "server sink got OnMessage" );

        if ( pSinkS->cMsg > 0 )
        {
            Check ( pSinkS->bstrTopic != NULL && ::wcscmp ( pSinkS->bstrTopic, L"chat" ) == 0
                  , "topic survived as a BSTR" );
            Check ( pSinkS->bstrSource != NULL && ::wcscmp ( pSinkS->bstrSource, L"Com.Client" ) == 0
                  , "source is the originating hub" );
            Check ( pSinkS->bBroadcast == VARIANT_FALSE, "unicast is not flagged broadcast" );
            Check ( pSinkS->payload.size() == ( ::wcslen ( L"hello com" ) + 1 ) * sizeof(wchar_t)
                  , "text payload arrived as a byte array, terminator included" );
            Check ( pSinkS->payload.size() >= sizeof(wchar_t) &&
                    ::wcscmp ( (const wchar_t*)&pSinkS->payload[0], L"hello com" ) == 0
                  , "text payload bytes are the original string" );
        }

        // --- binary round-trip (SAFEARRAY both ways) --------------------------
        BYTE blob[256];
        for ( int i = 0; i < 256; ++i ) blob[i] = (BYTE)i;

        VARIANT vBlob;
        MakeByteArray ( blob, 256, &vBlob );
        BSTR bstrBin = ::SysAllocString ( L"bin" );
        hr = pS->Send ( bstrC, bstrBin, vBlob );
        Check ( SUCCEEDED(hr), "Send(SAFEARRAY of 256 bytes)" );
        Check ( WaitUntil ( &pSinkC->cMsg, 1, 10000 ), "client sink got the binary message" );
        Check ( pSinkC->payload.size() == 256 &&
                ::memcmp ( &pSinkC->payload[0], blob, 256 ) == 0
              , "all 256 bytes round-tripped unchanged" );
        ::VariantClear ( &vBlob );

        // --- broadcast --------------------------------------------------------
        BSTR bstrNews = ::SysAllocString ( L"news" );
        VARIANT vNews;
        ::VariantInit ( &vNews );
        vNews.vt = VT_BSTR;
        vNews.bstrVal = ::SysAllocString ( L"to everyone" );

        VARIANT_BOOL vbDelivered = VARIANT_FALSE;
        hr = pS->Broadcast ( bstrNews, vNews, &vbDelivered );
        Check ( SUCCEEDED(hr) && vbDelivered == VARIANT_TRUE
              , "Broadcast reports delivery as a return value" );
        Check ( WaitUntil ( &pSinkC->cMsg, 2, 10000 ), "client sink got the broadcast" );
        Check ( pSinkC->bBroadcast == VARIANT_TRUE, "broadcast is flagged broadcast" );
        Check ( pSinkC->bstrTopic != NULL && ::wcscmp ( pSinkC->bstrTopic, L"news" ) == 0
              , "broadcast kept its topic" );
        ::VariantClear ( &vNews );

        // --- error contract ----------------------------------------------------
        BSTR bstrReserved = ::SysAllocString ( L"P2Pmsg_Nope" );
        hr = pC->SendText ( bstrS, bstrReserved, bstrHello );
        Check ( hr == kReservedTopic, "reserved topic returns P2PF_E_RESERVED_TOPIC" );
        ::SysFreeString ( bstrReserved );

        std::vector<BYTE> huge ( 30000, 0xAB );
        VARIANT vHuge;
        MakeByteArray ( &huge[0], (ULONG)huge.size(), &vHuge );
        hr = pC->Send ( bstrS, bstrTopic, vHuge );
        Check ( FAILED(hr), "oversize payload is refused, not put on the wire" );
        ::VariantClear ( &vHuge );

        VARIANT vBad;
        ::VariantInit ( &vBad );
        vBad.vt = VT_I4;
        vBad.lVal = 42;
        hr = pC->Send ( bstrS, bstrTopic, vBad );
        Check ( hr == DISP_E_TYPEMISMATCH, "a non-array, non-string payload is a type mismatch" );

        // --- the read side (dispids 9-13) ---------------------------------------
        //
        // This is the block that would have caught the sibling mesh.  The two
        // arms above returned P2PF_S_UNRELATED_LINK to THIS caller and S_OK to
        // every automation caller, so the question has to be answerable
        // afterwards -- and it is, from any tier, by asking the hub.
        BSTR bstrSub = ::SysAllocString ( L"Com.Server.Sub" );
        BSTR bstrEmpty = ::SysAllocString ( L"" );

        // A DESCENDANT arm, for contrast: same verb, same hub, S_OK exactly.
        // The endpoint is empty on purpose -- the facade resolves an in-process
        // Dmx service from the address pair, so nothing has to be configured.
        hr = pS->Listen ( bstrSub, bstrEmpty );
        Check ( hr == S_OK, "Listen(Com.Server.Sub, resolved) -> S_OK, not the sibling code" );

        LONG lFlags = 0;
        hr = pS->RelationTo ( bstrC, &lFlags );
        Check ( SUCCEEDED(hr) && ( lFlags & p2pfRelUnrelated ) != 0
              , "RelationTo(Com.Client) reports p2pfRelUnrelated" );
        Check ( SUCCEEDED(hr) && ( lFlags & p2pfConListen ) != 0
              , "RelationTo(Com.Client) reports what this hub armed: listen" );

        lFlags = 0;
        hr = pS->RelationTo ( bstrSub, &lFlags );
        Check ( SUCCEEDED(hr) && ( lFlags & p2pfRelDescendant ) != 0
              , "RelationTo(Com.Server.Sub) reports p2pfRelDescendant" );

        lFlags = 0;
        hr = pC->RelationTo ( bstrS, &lFlags );
        Check ( SUCCEEDED(hr) && ( lFlags & p2pfRelUnrelated ) != 0 &&
                                 ( lFlags & p2pfConDial ) != 0
              , "the dialing end sees the same edge as unrelated, and as a dial" );

        BSTR bstrNever = ::SysAllocString ( L"Com.NeverHeardOf" );
        Check ( pS->RelationTo ( bstrNever, &lFlags ) == kUnresolved
              , "RelationTo(unknown peer) is P2PF_E_UNRESOLVED" );
        ::SysFreeString ( bstrNever );

        LONG lCons = 0;
        hr = pS->get_ConCount ( &lCons );
        Check ( SUCCEEDED(hr) && lCons >= 2, "ConCount counts both armed peers" );

        BSTR bstrPeer0 = NULL;
        hr = pS->PeerAt ( 0, &bstrPeer0 );
        Check ( SUCCEEDED(hr) && bstrPeer0 != NULL && ::SysStringLen ( bstrPeer0 ) > 0
              , "PeerAt(0) is a peer name" );
        ::SysFreeString ( bstrPeer0 );

        BSTR bstrPastEnd = NULL;
        Check ( FAILED ( pS->PeerAt ( lCons, &bstrPastEnd ) ) && bstrPastEnd == NULL
              , "PeerAt past the end fails, and writes nothing" );
        Check ( pS->PeerAt ( -1, &bstrPastEnd ) == E_INVALIDARG
              , "a negative index is E_INVALIDARG, not an unsigned wrap" );

        // Canonically spelled, so it round-trips straight back into Listen.
        BSTR bstrEpBack = NULL;
        hr = pS->EndpointFor ( bstrC, &bstrEpBack );
        Check ( SUCCEEDED(hr) && bstrEpBack != NULL &&
                ::wcscmp ( bstrEpBack, L"tcp://:7799" ) == 0
              , "EndpointFor(Com.Client) round-trips the armed endpoint" );
        ::SysFreeString ( bstrEpBack );

        Check ( pS->EndpointFor ( bstrS, &bstrEpBack ) == kUnresolved
              , "EndpointFor(a peer this hub never heard of) is P2PF_E_UNRESOLVED" );

        // One string, one call -- the form a script or a bug report uses.
        BSTR bstrDesc = NULL;
        hr = pS->get_Description ( &bstrDesc );
        Check ( SUCCEEDED(hr) && bstrDesc != NULL, "Description is one BSTR snapshot" );
        if ( bstrDesc != NULL )
        {
            Check ( ::wcsstr ( bstrDesc, L"address=Com.Server\n" ) == bstrDesc
                  , "Description opens with this hub's own address" );
            Check ( ::wcsstr ( bstrDesc, L"con=Com.Client\ttcp://:7799\t" ) != NULL
                  , "Description carries peer, endpoint and flags, tab separated" );
            Check ( ::wcsstr ( bstrDesc, L"unrelated" )  != NULL &&
                    ::wcsstr ( bstrDesc, L"descendant" ) != NULL
                  , "Description spells both relations in words" );
            wprintf ( L"        --- Description ---\n%s", bstrDesc );
            ::SysFreeString ( bstrDesc );
        }

        // --- IP2PNetworkCom::Link (dispid 4) --------------------------------
        //
        // The one verb only the NETWORK can offer: it holds every live hub, so
        // it arms the listening side before the dialing one.  That ordering is
        // the folklore this removes -- a dmx:// dial does not retry, and a
        // script has no way to find that out except by having the link silently
        // not happen.
        BSTR bstrLA = ::SysAllocString ( L"Com.Link" );
        BSTR bstrLB = ::SysAllocString ( L"Com.Link.Peer" );
        IP2PHubCom *pLA = NULL, *pLB = NULL;

        Check ( SUCCEEDED ( pNet->CreateHub ( bstrLA, &pLA ) ) && pLA != NULL
              , "CreateHub(Com.Link)" );
        Check ( SUCCEEDED ( pNet->CreateHub ( bstrLB, &pLB ) ) && pLB != NULL
              , "CreateHub(Com.Link.Peer)" );

        if ( pLA != NULL && pLB != NULL )
        {
            // Parent and child, and no endpoint at all: the facade derives an
            // in-process Dmx service from the ordered pair.  S_OK exactly --
            // a descendant edge, so not the sibling code.
            hr = pNet->Link ( bstrLA, bstrLB, bstrEmpty );
            Check ( hr == S_OK, "Link(Com.Link -> Com.Link.Peer, resolved) -> S_OK" );

            // Both ends really were armed -- the check that separates Link from
            // a one-sided Listen that happens to return S_OK.
            VARIANT_BOOL vbLinked = VARIANT_FALSE;
            for ( int i = 0; i < 300; ++i )
            {
                pLA->IsPeerUp ( bstrLB, &vbLinked );
                if ( vbLinked == VARIANT_TRUE ) break;
                PumpOnce();
                ::Sleep ( 50 );
            }
            Check ( vbLinked == VARIANT_TRUE, "Link armed BOTH ends: the peer came up" );

            BSTR bstrLinkEp = NULL;
            hr = pLA->EndpointFor ( bstrLB, &bstrLinkEp );
            Check ( SUCCEEDED(hr) && bstrLinkEp != NULL &&
                    ::wcsncmp ( bstrLinkEp, L"dmx://", 6 ) == 0
                  , "the resolved endpoint is an in-process dmx:// service" );
            ::SysFreeString ( bstrLinkEp );

            // Twice for one pair is an error, not a no-op -- consistent with
            // the arming primitives, and the existing edge is left alone.
            Check ( pNet->Link ( bstrLA, bstrLB, bstrEmpty ) == kConDuplicate
                  , "a second Link on the same pair is P2PF_E_CON_DUPLICATE" );

            BSTR bstrGhost = ::SysAllocString ( L"Com.NoSuchHub" );
            Check ( pNet->Link ( bstrLA, bstrGhost, bstrEmpty ) == kNoHub
                  , "Link to an address no live hub answers to is P2PF_E_NO_HUB" );
            ::SysFreeString ( bstrGhost );
        }

        // --- CreateSecureHub (dispid 9), SecurityInfo (hub dispid 32) --------
        //
        // AUTHENTICATED HUBS, and this tier is the one that could not have
        // them any other way: everything the flag arranges -- an ECDSA
        // identity, its publishable point, an ECDH agreement key, a
        // three-column allow-list, a revocation list, an enforcement flag and
        // the kernel's arming gate -- is file paths, key containers and
        // 64-byte points, and a script can express none of it.
        //
        // WHAT IS ACTUALLY CHECKED IS THE POSTURE, not that the link works. A
        // secure hub that quietly fell back to a plain one would pass an
        // IsPeerUp check exactly as this one does, so the assertion that
        // carries the section is "armed=1" -- a hub can require authentication
        // and be unable to perform it, and that state refuses every peer.
        //
        // Note the verb that did NOT change: Link is still Link, with its three
        // arguments and its dispid. Security was settled when the two hubs were
        // created.
        {
            BSTR bstrSA = ::SysAllocString ( L"Com.Sec" );
            BSTR bstrSB = ::SysAllocString ( L"Com.Sec.Peer" );
            IP2PHubCom *pSA = NULL, *pSB = NULL;

            Check ( SUCCEEDED ( pNet->CreateSecureHub ( bstrSA, &pSA ) ) && pSA != NULL
                  , "CreateSecureHub(Com.Sec)" );
            Check ( SUCCEEDED ( pNet->CreateSecureHub ( bstrSB, &pSB ) ) && pSB != NULL
                  , "CreateSecureHub(Com.Sec.Peer)" );

            if ( pSA != NULL && pSB != NULL )
            {
                hr = pNet->Link ( bstrSA, bstrSB, bstrEmpty );
                Check ( hr == S_OK, "Link(Com.Sec -> Com.Sec.Peer) -> S_OK" );

                VARIANT_BOOL vbUp = VARIANT_FALSE;
                for ( int i = 0; i < 300; ++i )
                {
                    pSA->IsPeerUp ( bstrSB, &vbUp );
                    if ( vbUp == VARIANT_TRUE ) break;
                    PumpOnce();
                    ::Sleep ( 50 );
                }
                Check ( vbUp == VARIANT_TRUE
                      , "the peer came up, so the SIGNED login completed both ways" );

                BSTR bstrInfo = NULL;
                hr = pSA->get_SecurityInfo ( &bstrInfo );
                Check ( SUCCEEDED(hr) && bstrInfo != NULL &&
                        ::wcsstr ( bstrInfo, L"required=1" ) != NULL &&
                        ::wcsstr ( bstrInfo, L"armed=1"    ) != NULL
                      , "SecurityInfo says the hub requires auth AND can enforce it" );
                if ( bstrInfo != NULL )
                    wprintf ( L"        Com.Sec: %s\n", bstrInfo );
                ::SysFreeString ( bstrInfo );

                // BOTH SECURE OR NEITHER. Enforcement is hub-wide with no
                // per-connection override, so a secure hub demands a login a
                // plain one holds no key to produce; the refusal arrives
                // before anything is armed.
                Check ( pNet->Link ( bstrSA, bstrLB, bstrEmpty ) == kSecurity
                      , "a secure hub cannot be linked to a plain one" );
                Check ( pNet->Link ( bstrLA, bstrSB, bstrEmpty ) == kSecurity
                      , "...and it is refused the same way from the other side" );

                // A PLAIN hub answers the same question with zeros rather than
                // raising: asking whether something is secure should not be an
                // exception.
                BSTR bstrPlain = NULL;
                Check ( SUCCEEDED ( pLA->get_SecurityInfo ( &bstrPlain ) ) &&
                        bstrPlain != NULL &&
                        ::wcsstr ( bstrPlain, L"required=0" ) != NULL &&
                        ::wcsstr ( bstrPlain, L"(none)"     ) != NULL
                      , "a plain hub reads back as holding no identity" );
                ::SysFreeString ( bstrPlain );

                pSA->Close();
                pSB->Close();
            }

            if ( pSA != NULL ) pSA->Release();
            if ( pSB != NULL ) pSB->Release();
            ::SysFreeString ( bstrSA );
            ::SysFreeString ( bstrSB );
        }

        // --- IP2PNetworkCom: the deployment map (dispids 5-7) ----------------
        //
        // The endpoint has been a string since ABI 4; this is the first release
        // in which a caller can put that string somewhere OTHER than the call
        // site.  It matters most here: a script cannot be recompiled with a new
        // constant, so "where the server lives" had to be edited into the
        // script itself.
        {
            BSTR bstrMap = ::SysAllocString (
                L"# peers.ini -- where each address lives\n"
                L"\n"
                L"  Com.Cfg = TCP://127.0.0.1:7813\n"
                L"; and a semicolon comment\n" );
            BSTR bstrCfgS  = ::SysAllocString ( L"Com.Cfg" );
            BSTR bstrCfgC  = ::SysAllocString ( L"Com.Cfg.Client" );
            BSTR bstrUnset = ::SysAllocString ( L"Com.Cfg.Nobody" );

            Check ( pNet->SetEndpointMap ( bstrMap ) == S_OK
                  , "SetEndpointMap accepts comments, blanks and indentation" );

            BSTR bstrBack = NULL;
            Check ( pNet->EndpointFor ( bstrCfgS, &bstrBack ) == S_OK &&
                    bstrBack != NULL &&
                    ::wcscmp ( bstrBack, L"tcp://127.0.0.1:7813" ) == 0
                  , "...and reads back canonically, not as it was spelled" );
            ::SysFreeString ( bstrBack ); bstrBack = NULL;

            // An empty string, NOT an error: from automation, asking whether an
            // address is configured is an ordinary question and must not raise.
            Check ( pNet->EndpointFor ( bstrUnset, &bstrBack ) == S_OK &&
                    bstrBack != NULL && ::SysStringLen ( bstrBack ) == 0
                  , "...and an unconfigured address answers empty, not an error" );
            ::SysFreeString ( bstrBack ); bstrBack = NULL;

            // Neither arming call names an endpoint. This is the whole tier.
            IP2PHubCom *pCfgS = NULL, *pCfgC = NULL;
            Check ( SUCCEEDED ( pNet->CreateHub ( bstrCfgS, &pCfgS ) ) && pCfgS
                  , "CreateHub(Com.Cfg)" );
            Check ( SUCCEEDED ( pNet->CreateHub ( bstrCfgC, &pCfgC ) ) && pCfgC
                  , "CreateHub(Com.Cfg.Client)" );

            if ( pCfgS != NULL && pCfgC != NULL )
            {
                Check ( pCfgS->Listen  ( bstrCfgC, bstrEmpty ) == S_OK &&
                        pCfgC->Connect ( bstrCfgS, bstrEmpty ) == S_OK
                      , "Listen and Connect with NO endpoint at either call site" );

                VARIANT_BOOL vbUp = VARIANT_FALSE;
                for ( int i = 0; i < 300 && vbUp != VARIANT_TRUE; ++i )
                {
                    PumpOnce();
                    ::Sleep ( 50 );
                    pCfgC->IsPeerUp ( bstrCfgS, &vbUp );
                }
                Check ( vbUp == VARIANT_TRUE, "...linked up over the CONFIGURED tcp endpoint" );

                Check ( pCfgS->EndpointFor ( bstrCfgC, &bstrBack ) == S_OK &&
                        bstrBack != NULL &&
                        ::wcscmp ( bstrBack, L"tcp://:7813" ) == 0
                      , "...the listener took the entry with the host dropped" );
                ::SysFreeString ( bstrBack ); bstrBack = NULL;
            }

            // A bad line comes back as a LINE NUMBER in Err.Description. That
            // is the point of validating at set time: the alternative is a
            // Connect failing later, about a file it never mentions.
            BSTR bstrBadMap = ::SysAllocString ( L"A = tcp://h:1\nB = htp://nope\n" );
            Check ( pNet->SetEndpointMap ( bstrBadMap ) == kEndpoint
                  , "a malformed map is P2PF_E_ENDPOINT" );

            IErrorInfo *pEiMap = NULL;
            if ( ::GetErrorInfo ( 0, &pEiMap ) == S_OK && pEiMap != NULL )
            {
                BSTR bstrWhyMap = NULL;
                pEiMap->GetDescription ( &bstrWhyMap );
                Check ( bstrWhyMap != NULL && ::wcsstr ( bstrWhyMap, L"line 2" ) != NULL
                      , "...and Err.Description names the offending LINE" );
                if ( bstrWhyMap != NULL )
                    printf ( "        %ls\n", bstrWhyMap );
                ::SysFreeString ( bstrWhyMap );
                pEiMap->Release();
            }
            else
                Check ( false, "...and Err.Description names the offending LINE" );

            // All-or-nothing: the refused block changed nothing.
            Check ( pNet->EndpointFor ( bstrCfgS, &bstrBack ) == S_OK &&
                    bstrBack != NULL &&
                    ::wcscmp ( bstrBack, L"tcp://127.0.0.1:7813" ) == 0
                  , "...and the map already loaded is untouched" );
            ::SysFreeString ( bstrBack ); bstrBack = NULL;

            // One entry at a time, and an empty endpoint removes it.
            BSTR bstrOne = ::SysAllocString ( L"Com.Cfg.Late" );
            BSTR bstrPipe = ::SysAllocString ( L"pipe://TargetComCfgLate" );
            Check ( pNet->SetEndpoint ( bstrOne, bstrPipe ) == S_OK &&
                    pNet->EndpointFor ( bstrOne, &bstrBack ) == S_OK &&
                    bstrBack != NULL && ::wcscmp ( bstrBack, L"pipe://TargetComCfgLate" ) == 0
                  , "SetEndpoint adds one entry" );
            ::SysFreeString ( bstrBack ); bstrBack = NULL;

            Check ( pNet->SetEndpoint ( bstrOne, bstrEmpty ) == S_OK &&
                    pNet->EndpointFor ( bstrOne, &bstrBack ) == S_OK &&
                    bstrBack != NULL && ::SysStringLen ( bstrBack ) == 0
                  , "...and an empty endpoint removes it" );
            ::SysFreeString ( bstrBack ); bstrBack = NULL;

            // Put the process back: the map is network-wide, so anything armed
            // after this section would otherwise resolve against it.
            if ( pCfgC != NULL ) { pCfgC->Close(); pCfgC->Release(); }
            if ( pCfgS != NULL ) { pCfgS->Close(); pCfgS->Release(); }
            Check ( pNet->SetEndpointMap ( bstrEmpty ) == S_OK
                  , "empty text clears the map" );

            ::SysFreeString ( bstrMap );    ::SysFreeString ( bstrBadMap );
            ::SysFreeString ( bstrCfgS );   ::SysFreeString ( bstrCfgC );
            ::SysFreeString ( bstrUnset );  ::SysFreeString ( bstrOne );
            ::SysFreeString ( bstrPipe );
        }

        // Late-bindable, which is the whole reason any of it is here: the flat
        // ABI has had Link since ABI 2 and the map since ABI 5, and a script
        // reaches them only through IDispatch.
        IDispatch *pDispNet = NULL;
        if ( SUCCEEDED ( pNet->QueryInterface ( IID_IDispatch, (void**)&pDispNet ) ) &&
             pDispNet != NULL )
        {
            struct NamedDispid { OLECHAR *name; DISPID want; };
            NamedDispid aWanted[] =
            {
                { (OLECHAR*)L"Link",           4 },
                { (OLECHAR*)L"SetEndpoint",    5 },
                { (OLECHAR*)L"SetEndpointMap", 6 },
                { (OLECHAR*)L"EndpointFor",    7 },
            };
            int nDispidOk = 0;
            for ( int i = 0; i < 4; ++i )
            {
                DISPID dispid = 0;
                if ( SUCCEEDED ( pDispNet->GetIDsOfNames ( IID_NULL, &aWanted[i].name, 1
                                                         , LOCALE_USER_DEFAULT, &dispid ) ) &&
                     dispid == aWanted[i].want )
                    ++nDispidOk;
            }
            Check ( nDispidOk == 4
                  , "GetIDsOfNames resolves Link=4, SetEndpoint=5, Map=6, EndpointFor=7" );
            pDispNet->Release();
        }

        // --- IErrorInfo: the code carries a sentence now ---------------------
        //
        // The endpoint is a STRING, so its failures are runtime failures with
        // nothing typed at the call site to inspect.  0x80040208 on its own
        // tells a script author nothing about which part of "tpc://:7799" the
        // parser objected to -- so the layer says.
        ISupportErrorInfo *pSei = NULL;
        hr = pS->QueryInterface ( IID_ISupportErrorInfo, (void**)&pSei );
        Check ( SUCCEEDED(hr) && pSei != NULL, "the hub exposes ISupportErrorInfo" );
        if ( pSei != NULL )
        {
            Check ( pSei->InterfaceSupportsErrorInfo ( IID_IP2PHubCom ) == S_OK
                  , "...and claims rich errors for IP2PHubCom" );
            Check ( pSei->InterfaceSupportsErrorInfo ( IID_IP2PNetworkCom ) == S_FALSE
                  , "...and only for that one" );
            pSei->Release();
        }

        BSTR bstrTypo = ::SysAllocString ( L"tpc://:7799" );
        Check ( pS->Listen ( bstrC, bstrTypo ) == kEndpoint
              , "a mistyped scheme is P2PF_E_ENDPOINT" );

        IErrorInfo *pEi = NULL;
        Check ( ::GetErrorInfo ( 0, &pEi ) == S_OK && pEi != NULL
              , "...and it left an IErrorInfo behind" );
        if ( pEi != NULL )
        {
            BSTR bstrWhy = NULL, bstrSrc = NULL;
            pEi->GetDescription ( &bstrWhy );
            pEi->GetSource ( &bstrSrc );

            Check ( bstrWhy != NULL && ::SysStringLen ( bstrWhy ) > 0
                  , "Err.Description is not empty" );
            if ( bstrWhy != NULL )
            {
                // Both halves of the contract: what was rejected, and what
                // would have been accepted.
                Check ( ::wcsstr ( bstrWhy, L"tpc://:7799" ) != NULL
                      , "...it echoes the offending string back" );
                Check ( ::wcsstr ( bstrWhy, L"Listen" ) != NULL
                      , "...it names the call that failed" );
                Check ( ::wcsstr ( bstrWhy, L"tcp://" ) != NULL
                      , "...and it spells out the grammar that would work" );
                wprintf ( L"        --- Err.Description ---\n        %s\n", bstrWhy );
            }
            Check ( bstrSrc != NULL && ::SysStringLen ( bstrSrc ) > 0
                  , "Err.Source names the component" );

            ::SysFreeString ( bstrWhy );
            ::SysFreeString ( bstrSrc );
            pEi->Release();
        }
        ::SysFreeString ( bstrTypo );

        // The swapped-argument call -- both parameters are BSTR, so nothing but
        // this guard catches it, and E_INVALIDARG alone would not explain it.
        BSTR bstrSwapped = ::SysAllocString ( L"tcp://127.0.0.1:7799" );
        Check ( pS->Connect ( bstrSwapped, bstrC ) == E_INVALIDARG
              , "arguments swapped: a URI in the peer slot is E_INVALIDARG" );
        pEi = NULL;
        if ( ::GetErrorInfo ( 0, &pEi ) == S_OK && pEi != NULL )
        {
            BSTR bstrWhy = NULL;
            pEi->GetDescription ( &bstrWhy );
            Check ( bstrWhy != NULL && ::wcsstr ( bstrWhy, L"PEER slot" ) != NULL
                  , "...and the description says WHICH argument, not just \"invalid\"" );
            ::SysFreeString ( bstrWhy );
            pEi->Release();
        }
        else
        {
            Check ( false, "...and the description says WHICH argument, not just \"invalid\"" );
        }
        ::SysFreeString ( bstrSwapped );

        // --- C++ automation: the same call down both paths ----------------------
        //
        // THE MEASUREMENT, not a feature test.  Everything above this point is a
        // vtable client; this block is the same process talking to the same hub
        // through IDispatch::Invoke, which is what a VB/VBA/scripting host does
        // and what ATL's IDispatchImpl runs a [dual] interface through.
        //
        // Three arms, in order:
        //   1. vtable   + sibling peer -> expect P2PF_S_UNRELATED_LINK
        //   2. Invoke   + sibling peer -> expect S_OK, and EXCEPINFO untouched.
        //      A DISTINCT peer address on purpose: repeating arm 1's peer would
        //      come back P2PF_E_CON_DUPLICATE and prove nothing.
        //   3. Invoke   + a REPEAT of arm 2's peer -> the control. A FAILURE
        //      HRESULT does survive Invoke, as DISP_E_EXCEPTION carrying the
        //      original code in EXCEPINFO::scode.
        //
        // Arms 2 and 3 together are the whole finding: ITypeInfo::Invoke
        // preserves every failure and discards every success, so a success code
        // is unreachable from automation BY DESIGN, not by an oversight anywhere
        // in this layer.  Arm 4 then shows the way out -- RelationTo through the
        // very same Invoke, carrying the same fact as a value.
        IDispatch *pDisp = NULL;
        hr = pS->QueryInterface ( IID_IDispatch, (void**)&pDisp );
        Check ( SUCCEEDED(hr) && pDisp != NULL, "the hub answers QueryInterface(IID_IDispatch)" );

        if ( pDisp != NULL )
        {
            OLECHAR *szListen     = L"Listen";
            OLECHAR *szRelationTo = L"RelationTo";
            DISPID   dispidListen = 0, dispidRelationTo = 0;

            hr = pDisp->GetIDsOfNames ( IID_NULL, &szListen, 1, LOCALE_USER_DEFAULT, &dispidListen );
            Check ( SUCCEEDED(hr) && dispidListen == 1, "GetIDsOfNames(Listen) resolves to dispid 1" );

            hr = pDisp->GetIDsOfNames ( IID_NULL, &szRelationTo, 1, LOCALE_USER_DEFAULT, &dispidRelationTo );
            Check ( SUCCEEDED(hr) && dispidRelationTo == 9
                  , "GetIDsOfNames(RelationTo) resolves to dispid 9 -- the read side is late-bindable" );

            BSTR bstrProbeV = ::SysAllocString ( L"Com.ProbeVtbl" );
            BSTR bstrProbeD = ::SysAllocString ( L"Com.ProbeDisp" );

            // Arm 1 -- vtable.  Sibling of Com.Server, so the facade classifies.
            HRESULT hrVtbl = pS->Listen ( bstrProbeV, bstrEmpty );
            Check ( hrVtbl == kUnrelatedLink
                  , "vtable arm: Listen(sibling) returns P2PF_S_UNRELATED_LINK" );

            // Arm 2 -- IDispatch, arguments in REVERSE declaration order.
            VARIANT av[2];
            ::VariantInit ( &av[0] ); ::VariantInit ( &av[1] );
            av[0].vt = VT_BSTR; av[0].bstrVal = bstrEmpty;    // endpoint (2nd param)
            av[1].vt = VT_BSTR; av[1].bstrVal = bstrProbeD;   // toPeer   (1st param)

            DISPPARAMS dp;
            ::ZeroMemory ( &dp, sizeof(dp) );
            dp.rgvarg = av;
            dp.cArgs  = 2;

            EXCEPINFO ei;
            ::ZeroMemory ( &ei, sizeof(ei) );
            UINT uArgErr = 0;

            HRESULT hrDisp = pDisp->Invoke ( dispidListen, IID_NULL, LOCALE_USER_DEFAULT
                                           , DISPATCH_METHOD, &dp, NULL, &ei, &uArgErr );

            printf ( "        vtable  Listen -> 0x%08lX\n", hrVtbl );
            printf ( "        Invoke  Listen -> 0x%08lX (EXCEPINFO::scode 0x%08lX, wCode %u)\n"
                   , hrDisp, (LONG)ei.scode, (unsigned)ei.wCode );

            Check ( hrDisp == S_OK
                  , "automation arm: the SAME success code arrives as a flat S_OK" );
            Check ( ei.scode == 0 && ei.wCode == 0
                  , "...and nothing was left in EXCEPINFO either -- the value is simply gone" );
            ::SysFreeString ( ei.bstrSource ); ::SysFreeString ( ei.bstrDescription );
            ::SysFreeString ( ei.bstrHelpFile );

            // Arm 3 -- the control.  Same call, same peer, now a duplicate.
            ::ZeroMemory ( &ei, sizeof(ei) );
            HRESULT hrDup = pDisp->Invoke ( dispidListen, IID_NULL, LOCALE_USER_DEFAULT
                                          , DISPATCH_METHOD, &dp, NULL, &ei, &uArgErr );
            printf ( "        Invoke  Listen (duplicate) -> 0x%08lX (EXCEPINFO::scode 0x%08lX)\n"
                   , hrDup, (LONG)ei.scode );
            Check ( ( hrDup == DISP_E_EXCEPTION && ei.scode == kConDuplicate ) ||
                      hrDup == kConDuplicate
                  , "control: a FAILURE code does survive Invoke, in EXCEPINFO::scode" );

            // ...and it now survives with a SENTENCE attached.  ITypeInfo::Invoke
            // copies the object's IErrorInfo into EXCEPINFO, and EXCEPINFO is
            // precisely what a scripting host turns into Err.Description -- so
            // this one check stands in for every VB, VBA and VBScript client.
            if ( ei.bstrDescription != NULL )
                wprintf ( L"        EXCEPINFO::bstrDescription = %s\n", ei.bstrDescription );
            Check ( hrDup != DISP_E_EXCEPTION ||
                    ( ei.bstrDescription != NULL &&
                      ::wcsstr ( ei.bstrDescription, L"already has a connection" ) != NULL )
                  , "automation gets the EXPLANATION too, in EXCEPINFO::bstrDescription" );
            ::SysFreeString ( ei.bstrSource ); ::SysFreeString ( ei.bstrDescription );
            ::SysFreeString ( ei.bstrHelpFile );

            // Arm 4 -- the way out.  One [in], one [out,retval], through the
            // identical Invoke that discarded the code above.
            VARIANT avRel[1];
            ::VariantInit ( &avRel[0] );
            avRel[0].vt = VT_BSTR; avRel[0].bstrVal = bstrProbeD;

            DISPPARAMS dpRel;
            ::ZeroMemory ( &dpRel, sizeof(dpRel) );
            dpRel.rgvarg = avRel;
            dpRel.cArgs  = 1;

            VARIANT vFlags;
            ::VariantInit ( &vFlags );
            ::ZeroMemory ( &ei, sizeof(ei) );

            hr = pDisp->Invoke ( dispidRelationTo, IID_NULL, LOCALE_USER_DEFAULT
                               , DISPATCH_METHOD, &dpRel, &vFlags, &ei, &uArgErr );
            printf ( "        Invoke  RelationTo -> 0x%08lX (vt=%u, lVal=0x%04lX)\n"
                   , hr, (unsigned)vFlags.vt, (LONG)vFlags.lVal );
            Check ( SUCCEEDED(hr) && vFlags.vt == VT_I4 &&
                    ( vFlags.lVal & p2pfRelUnrelated ) != 0
                  , "the read side DOES reach automation: RelationTo returns the flags as a value" );
            ::SysFreeString ( ei.bstrSource ); ::SysFreeString ( ei.bstrDescription );
            ::SysFreeString ( ei.bstrHelpFile );
            ::VariantClear ( &vFlags );

            // The BSTRs above are owned by this frame, not by the VARIANTs.
            ::SysFreeString ( bstrProbeV );
            ::SysFreeString ( bstrProbeD );
            pDisp->Release();
        }

        // --- past the messaging slice (facade ABI 6, dispids 14-20) --------------
        //
        // Seven methods and two events. What this block is really testing is
        // whether the facade's newest surface survives the trip through a
        // QUEUED, marshalled, apartment-crossing event tier -- which is the one
        // thing the flat suite cannot check, and the reason OnMessageEx's
        // answer is the one addition that could not come across at all.
        printf ( "\n-- past the messaging slice --\n" );
        {
            LONG lRtt = -1;
            HRESULT hrPing = pS->Ping ( bstrC, 5000, &lRtt );
            Check ( SUCCEEDED(hrPing) && lRtt >= 0 && lRtt <= 5000
                  , "Ping() completes a round trip and reports it in ms" );
            printf ( "        round trip %ld ms\n", lRtt );

            BSTR bstrGhost = ::SysAllocString ( L"Com.NoSuchPeer" );
            LONG lNever = 0;
            Check ( pS->Ping ( bstrGhost, 400, &lNever ) == kTimeout
                  , "...and P2PF_E_TIMEOUT when nothing answers" );

            // The knobs. MODE is the kernel object's own answer, so it is the
            // check that proves this reaches past the facade's bookkeeping.
            LONG lMode = 0;
            Check ( SUCCEEDED ( pC->GetConOption ( bstrS, p2pfOptMode, &lMode ) ) &&
                    lMode == p2pfModeDial
                  , "GetConOption reports the dialing side's role" );

            LONG lState = 0;
            Check ( SUCCEEDED ( pC->GetConOption ( bstrS, p2pfOptConState, &lState ) ) &&
                    ( lState & p2pfStateLogin ) != 0
                  , "...and the connection state bits, with Login set" );

            // The LISTENING end of the same edge must agree. It holds two
            // connections for this peer -- the service it armed and the clone
            // the kernel spawned on accept -- and only the clone logs in. The
            // kernel's own lookup returns the service, so this used to read
            // 0x0003; the facade now picks the connection carrying the session.
            LONG lStateS = 0;
            Check ( SUCCEEDED ( pS->GetConOption ( bstrC, p2pfOptConState, &lStateS ) ) &&
                    ( lStateS & p2pfStateLogin ) != 0
                  , "...and the LISTENING end reports the same, not the idle service" );
            Check ( SUCCEEDED ( pS->GetConOption ( bstrC, p2pfOptMode, &lMode ) ) &&
                    lMode == p2pfModeListen
                  , "...while Mode still reports what that hub armed" );

            LONG lTrace = 0;
            Check ( SUCCEEDED ( pC->SetConOption ( bstrS, p2pfOptTrace, 1 ) ) &&
                    SUCCEEDED ( pC->GetConOption ( bstrS, p2pfOptTrace, &lTrace ) ) &&
                    lTrace == 1
                  , "SetConOption writes one no tier could reach before" );
            pC->SetConOption ( bstrS, p2pfOptTrace, 0 );

            Check ( pC->SetConOption ( bstrS, p2pfOptMode, 1 ) == kOption
                  , "...a write to a read-only option is refused" );
            Check ( pC->GetConOption ( bstrGhost, p2pfOptMode, &lMode ) == kNoPeer
                  , "...and a peer this hub has no connection for" );

            // A timer, all the way out to a marshalled event on this STA.
            LONG lTimerId = 0;
            Check ( SUCCEEDED ( pS->SetTimer ( 10, 0x51, &lTimerId ) ) && lTimerId != 0
                  , "SetTimer() returns the id its event will carry" );
            Check ( WaitUntil ( &pSinkS->cTimer, 1, 15000 )
                  , "...and OnTimer arrives on the sink, marshalled" );
            Check ( pSinkS->lTimerId == lTimerId && pSinkS->lTimerKey == 0x51
                  , "...carrying that id and the key it was given" );

            LONG lLong = 0;
            Check ( SUCCEEDED ( pS->SetTimer ( 60000, 1, &lLong ) ) &&
                    pS->KillTimer ( lLong ) == S_OK &&
                    pS->KillTimer ( 999999 ) == S_FALSE
                  , "KillTimer accepts a live timer and declines an unknown id" );

            // OnEvent: the sibling arm earlier in this suite already raised the
            // unrelated-link report, so the structured twin is here too -- with
            // a code to branch on instead of a sentence to parse.
            Check ( pSinkS->cEvent > 0 || pSinkC->cEvent > 0
                  , "OnEvent reached the sink beside OnError" );
            Check ( pSinkS->cEventUnrelated > 0 || pSinkC->cEventUnrelated > 0
                  , "...as a P2PEventCode, which is what a client can switch on" );
            Check ( pSinkS->cError > 0 || pSinkC->cError > 0
                  , "...and OnError still fired, so published sinks are unbroken" );

            // --- the message model (facade ABI 7, dispids 21-26) ------------
            //
            // The interesting property is not that a tag round-trips -- the
            // flat suite proves that over a real wire. It is that the four
            // read properties answer AT ALL here, when the message they
            // describe was destroyed on the pump thread long before the sink
            // in this apartment ever ran. They work because the facade is
            // asked at enqueue time and because the gate is a causality id
            // rather than a thread id; a thread compare would fail every one
            // of these, since the read arrives on an RPC pool thread.
            printf ( "\n-- the message model --\n" );

            BSTR bstrTagTopic = ::SysAllocString ( L"tagged" );
            VARIANT vTagged;  ::VariantInit ( &vTagged );
            vTagged.vt = VT_BSTR;
            vTagged.bstrVal = ::SysAllocString ( L"body" );

            LONG lWant = pSinkS->cMsg + 1;
            Check ( SUCCEEDED ( pC->SendEx ( bstrS, bstrTagTopic, vTagged,
                                             0x2BAD, p2pfPriHigh, 0 ) ) &&
                    WaitUntil ( &pSinkS->cMsg, lWant, 10000 )
                  , "SendEx delivers through the queued, marshalled event tier" );

            Check ( SUCCEEDED ( pSinkS->hrMsgTag ) && pSinkS->lMsgTag == 0x2BAD
                  , "...and MsgTag answers inside the handler, in another apartment" );
            Check ( pSinkS->lMsgPriority == p2pfPriHigh
                  , "...MsgPriority carries what the sender set" );
            Check ( SUCCEEDED ( pSinkS->hrMsgDest ) && pSinkS->bstrMsgDest != NULL &&
                    ::wcscmp ( pSinkS->bstrMsgDest, L"Com.Server" ) == 0
                  , "...and MsgDest names the addressee" );
            Check ( ( pSinkS->lMsgFlags & p2pfMsgBounces   ) != 0 &&
                    ( pSinkS->lMsgFlags & p2pfMsgBroadcast ) == 0
                  , "...MsgFlags reports bounces-on and not-broadcast" );

            // OnPeerUp ran long ago on this same object and asked the same
            // question. THAT is the gate doing its job.
            Check ( pSinkS->hrMsgOutside == kNoMessage
                  , "the same property outside a message event is p2pfNoMessage" );

            LONG lJunk = 0;
            Check ( pS->get_MsgTag ( &lJunk ) == kNoMessage
                  , "...and so is a read from the client's own thread" );

            lWant = pSinkC->cMsg + 1;
            VARIANT_BOOL vbSent = VARIANT_FALSE;
            Check ( SUCCEEDED ( pS->BroadcastEx ( bstrTagTopic, vTagged,
                                                  0x900D, p2pfPriDefault, 0,
                                                  &vbSent ) ) &&
                    vbSent == VARIANT_TRUE &&
                    WaitUntil ( &pSinkC->cMsg, lWant, 10000 )
                  , "BroadcastEx delivers, and reports that a peer was up" );
            Check ( pSinkC->lMsgTag == 0x900D &&
                    ( pSinkC->lMsgFlags & p2pfMsgBroadcast ) != 0
                  , "...carrying its tag, and flagged as a broadcast" );

            ::VariantClear ( &vTagged );
            ::SysFreeString ( bstrTagTopic );

            // --- named fields (facade ABI 8, dispids 27-31) ----------------
            //
            // A record with structure, sent from script with no encoding
            // agreed at either end -- which is the whole point -- and read
            // back inside a handler in another apartment.
            printf ( "\n-- named fields --\n" );

            IP2PMessageCom *pMsgObj = NULL;
            Check ( SUCCEEDED ( pNet->CreateMessage ( &pMsgObj ) ) && pMsgObj != NULL
                  , "CreateMessage makes one, from the NETWORK not a hub" );

            if ( pMsgObj != NULL )
            {
                BSTR bsWho  = ::SysAllocString ( L"who" );
                BSTR bsBin  = ::SysAllocString ( L"bin" );
                BSTR bsVal  = ::SysAllocString ( L"com-node" );
                VARIANT vBin; ::VariantInit ( &vBin );
                {
                    SAFEARRAY *psa = ::SafeArrayCreateVector ( VT_UI1, 0, 3 );
                    BYTE *p = NULL;
                    ::SafeArrayAccessData ( psa, (void**)&p );
                    p[0] = 0x00; p[1] = 0xFF; p[2] = 0x7F;
                    ::SafeArrayUnaccessData ( psa );
                    vBin.vt = VT_ARRAY | VT_UI1;
                    vBin.parray = psa;
                }
                VARIANT vBody; ::VariantInit ( &vBody );
                vBody.vt = VT_BSTR; vBody.bstrVal = ::SysAllocString ( L"body" );

                Check ( SUCCEEDED ( pMsgObj->SetFieldText ( bsWho, bsVal ) ) &&
                        SUCCEEDED ( pMsgObj->SetField ( bsBin, vBin ) ) &&
                        SUCCEEDED ( pMsgObj->SetPayload ( vBody ) )
                      , "SetFieldText / SetField / SetPayload are accepted" );

                LONG lCount = 0;
                Check ( SUCCEEDED ( pMsgObj->get_FieldCount ( &lCount ) ) && lCount == 2
                      , "...and FieldCount agrees" );

                // Read one back off the SEND object -- the write side's own
                // reader, which is a different thing from MsgField below.
                VARIANT vBack; ::VariantInit ( &vBack );
                Check ( SUCCEEDED ( pMsgObj->GetField ( bsBin, &vBack ) ) &&
                        ( vBack.vt & VT_ARRAY ) != 0
                      , "GetField reads one back off the message object" );
                ::VariantClear ( &vBack );

                LONG lWantF = pSinkS->cMsg + 1;
                BSTR bsRec  = ::SysAllocString ( L"rec" );
                Check ( SUCCEEDED ( pC->SendMsg ( bstrS, bsRec, pMsgObj, 0x77, -1, 0 ) ) &&
                        WaitUntil ( &pSinkS->cMsg, lWantF, 10000 )
                      , "SendMsg delivers it through the queued event tier" );

                Check ( pSinkS->lMsgFieldCount == 2
                      , "...MsgFieldCount answers inside the handler" );
                Check ( pSinkS->bstrField0 != NULL &&
                        ::wcscmp ( pSinkS->bstrField0, L"who" ) == 0
                      , "...MsgFieldName gives the sender's order" );
                Check ( SUCCEEDED ( pSinkS->hrMsgField ) &&
                        ( pSinkS->vMsgField.vt & VT_ARRAY ) != 0
                      , "...MsgField hands the value over as a byte array" );
                Check ( pSinkS->hrMsgFieldAbsent == kNoField
                      , "...and an ABSENT field is p2pfNoField, not an empty one" );

                // The same object again: not consumed by sending.
                lWantF = pSinkS->cMsg + 1;
                Check ( SUCCEEDED ( pC->SendMsg ( bstrS, bsRec, pMsgObj, 0x78, -1, 0 ) ) &&
                        WaitUntil ( &pSinkS->cMsg, lWantF, 10000 ) &&
                        pSinkS->lMsgFieldCount == 2
                      , "the same message object sends again unchanged" );

                VARIANT_BOOL vbF = VARIANT_FALSE;
                Check ( SUCCEEDED ( pS->BroadcastMsg ( bsRec, pMsgObj, 0x79, -1, 0, &vbF ) )
                      , "BroadcastMsg is accepted" );

                VARIANT_BOOL vbRemoved = VARIANT_FALSE;
                Check ( SUCCEEDED ( pMsgObj->RemoveField ( bsWho, &vbRemoved ) ) &&
                        vbRemoved == VARIANT_TRUE &&
                        SUCCEEDED ( pMsgObj->RemoveField ( bsWho, &vbRemoved ) ) &&
                        vbRemoved == VARIANT_FALSE
                      , "RemoveField answers True then False, not an error" );

                Check ( SUCCEEDED ( pMsgObj->Clear() ) &&
                        SUCCEEDED ( pMsgObj->get_FieldCount ( &lCount ) ) && lCount == 0
                      , "Clear empties it for reuse" );

                LONG lJunkF = 0;
                Check ( pS->get_MsgFieldCount ( &lJunkF ) == kNoMessage
                      , "MsgFieldCount outside a message event is p2pfNoMessage" );

                ::VariantClear ( &vBin ); ::VariantClear ( &vBody );
                ::SysFreeString ( bsWho ); ::SysFreeString ( bsBin );
                ::SysFreeString ( bsVal ); ::SysFreeString ( bsRec );
                pMsgObj->Release();
            }

            Check ( SUCCEEDED ( pS->CloseIdleCons() )
                  , "CloseIdleCons is accepted" );

            // Last, because it takes the link down.
            Check ( pS->Disconnect ( bstrGhost ) == kNoPeer
                  , "Disconnect of an unknown peer says so" );
            Check ( SUCCEEDED ( pS->Disconnect ( bstrC ) )
                  , "Disconnect drops one peer" );
            VARIANT_BOOL vbUp = VARIANT_TRUE;
            Check ( SUCCEEDED ( pS->IsPeerUp ( bstrC, &vbUp ) ) && vbUp == VARIANT_FALSE
                  , "...the hub agrees it is down, and is still usable" );

            ::SysFreeString ( bstrGhost );
        }

        // --- teardown -----------------------------------------------------------
        if ( pCpS ) { Check ( SUCCEEDED ( pCpS->Unadvise ( dwCookieS ) ), "Unadvise(server sink)" );
                      Check ( pCpS->Unadvise ( dwCookieS ) == CONNECT_E_NOCONNECTION
                            , "double Unadvise is refused" );
                      pCpS->Release(); }
        if ( pCpC ) { pCpC->Unadvise ( dwCookieC ); pCpC->Release(); }

        Check ( SUCCEEDED ( pC->Close() ), "Close(client hub)" );
        Check ( SUCCEEDED ( pS->Close() ), "Close(server hub)" );
        Check ( pS->Listen ( bstrC, bstrEpListen ) == kClosed, "a closed hub returns P2PF_E_CLOSED" );
        Check ( SUCCEEDED ( pS->Close() ), "Close is idempotent" );

        pSinkS->Release();
        pSinkC->Release();

        // The Link pair closes here with everything else, never section by
        // section: a hub owning an armed-but-never-connected Dmx listener
        // leaves a stale entry in a kernel global list on teardown, and closing
        // in stages makes a LATER section fail in a way that reads like an
        // arming race.  See UnificationPlan_progress.md section 10.
        if ( pLB != NULL ) { pLB->Close(); pLB->Release(); }
        if ( pLA != NULL ) { pLA->Close(); pLA->Release(); }

        ::SysFreeString ( bstrS );        ::SysFreeString ( bstrC );
        ::SysFreeString ( bstrSub );      ::SysFreeString ( bstrEmpty );
        ::SysFreeString ( bstrEpListen ); ::SysFreeString ( bstrEpDial );
        ::SysFreeString ( bstrTopic );    ::SysFreeString ( bstrHello );
        ::SysFreeString ( bstrBin );      ::SysFreeString ( bstrNews );
        ::SysFreeString ( bstrLA );       ::SysFreeString ( bstrLB );

        pC->Release();
        pS->Release();
        pNet->Release();
    }

    ::CoUninitialize();

    printf ( "\n%d checks, %d failure(s)\n", g_checks, g_fails );
    return g_fails;
}
