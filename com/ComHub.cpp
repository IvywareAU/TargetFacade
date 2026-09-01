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
// ComHub.cpp -- implementation of coclass P2PHub.
#include "stdafx.h"
#include "ComHub.h"
#include "ComNetwork.h"
#include "ComMessage.h"

// Bound on the pending-event queue.  The kernel pump must never block on a
// slow client handler, so a backlog is dropped rather than accumulated; the
// drop count is reported to the client as an OnError once the queue drains.
static const size_t kMaxQueued = 4096;

// How long Close() waits for the dispatch thread before handing the teardown
// over to that thread.  Only ever hit when the client calls Close() from
// inside its own event handler.
static const DWORD  kCloseJoinMs = 10000;

// ---------------------------------------------------------------------------
// The process-wide Global Interface Table, created on first use.
// ---------------------------------------------------------------------------
static IGlobalInterfaceTable *g_pGit = NULL;

static IGlobalInterfaceTable* GetGit ( )
{
    if ( g_pGit == NULL )
    {
        IGlobalInterfaceTable *pGit = NULL;
        HRESULT hr = ::CoCreateInstance ( CLSID_StdGlobalInterfaceTable, NULL
                                        , CLSCTX_INPROC_SERVER
                                        , IID_IGlobalInterfaceTable
                                        , (void**)&pGit );
        if ( SUCCEEDED(hr) )
        {
            if ( ::InterlockedCompareExchangePointer ( (PVOID*)&g_pGit, pGit, NULL ) != NULL )
                pGit->Release();                     // lost the race
        }
    }
    return g_pGit;
}

// ---------------------------------------------------------------------------
// VARIANT <-> bytes
// ---------------------------------------------------------------------------
HRESULT VariantToBytes ( const VARIANT& vIn, std::vector<BYTE>& out )
{
    out.clear();

    const VARIANT *pv = &vIn;
    if ( pv->vt == (VT_VARIANT | VT_BYREF) && pv->pvarVal != NULL )
        pv = pv->pvarVal;

    if ( pv->vt == VT_EMPTY || pv->vt == VT_NULL )
        return S_OK;                                  // legitimately empty

    if ( pv->vt == VT_BSTR )
    {
        // Same convention as SendText: the string INCLUDING its terminator.
        LPCWSTR wsz = ( pv->bstrVal != NULL ) ? pv->bstrVal : L"";
        UINT cch = ( pv->bstrVal != NULL ) ? ::SysStringLen ( pv->bstrVal )
                                           : 0;
        const BYTE *p = (const BYTE*)wsz;
        out.assign ( p, p + ( (size_t)cch + 1 ) * sizeof(wchar_t) );
        return S_OK;
    }

    if ( ( pv->vt & VT_ARRAY ) && ( pv->vt & VT_TYPEMASK ) == VT_UI1 )
    {
        SAFEARRAY *psa = ( pv->vt & VT_BYREF ) ? ( pv->pparray ? *pv->pparray : NULL )
                                               : pv->parray;
        if ( psa == NULL )
            return S_OK;
        if ( ::SafeArrayGetDim ( psa ) != 1 )
            return DISP_E_TYPEMISMATCH;

        LONG lo = 0, hi = -1;
        HRESULT hr = ::SafeArrayGetLBound ( psa, 1, &lo );
        if ( SUCCEEDED(hr) ) hr = ::SafeArrayGetUBound ( psa, 1, &hi );
        if ( FAILED(hr) ) return hr;
        if ( hi < lo ) return S_OK;

        void *pData = NULL;
        hr = ::SafeArrayAccessData ( psa, &pData );
        if ( FAILED(hr) ) return hr;
        const BYTE *p = (const BYTE*)pData;
        out.assign ( p, p + ( (size_t)hi - lo + 1 ) );
        ::SafeArrayUnaccessData ( psa );
        return S_OK;
    }

    return DISP_E_TYPEMISMATCH;
}

HRESULT BytesToVariant ( const void *pData, unsigned int cb, VARIANT *pOut )
{
    if ( pOut == NULL ) return E_POINTER;
    ::VariantInit ( pOut );

    SAFEARRAY *psa = ::SafeArrayCreateVector ( VT_UI1, 0, cb );
    if ( psa == NULL ) return E_OUTOFMEMORY;

    if ( cb != 0 )
    {
        void *pDst = NULL;
        HRESULT hr = ::SafeArrayAccessData ( psa, &pDst );
        if ( FAILED(hr) ) { ::SafeArrayDestroy ( psa ); return hr; }
        ::memcpy ( pDst, pData, cb );
        ::SafeArrayUnaccessData ( psa );
    }

    pOut->vt     = VT_ARRAY | VT_UI1;
    pOut->parray = psa;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IErrorInfo -- one sentence per HRESULT this layer can hand back.
//
// The facade returns codes; a script author needs prose.  Everything below is
// the COM tier explaining the flat tier's answer, and the endpoint family is
// why it exists: "the endpoint is a string" (which is what makes a transport a
// configurable VALUE) means its errors are runtime errors with nothing typed at
// the call site to inspect.
//
// The rule for the wording: say what was rejected AND what would be accepted.
// A client that only learns "invalid" has to go and find the grammar; one that
// is told the grammar can fix the string and move on.
// ---------------------------------------------------------------------------
static LPCWSTR MeaningOf ( HRESULT hr )
{
    switch ( hr )
    {
      case p2pf::P2PF_E_ENDPOINT:
        return L"the endpoint could not be parsed, or names a transport this build "
               L"does not support. Accepted: \"tcp://:PORT\" to listen and "
               L"\"tcp://HOST:PORT\" to dial (a listen may not name a host -- the "
               L"kernel always binds every interface -- and a dial must; port 1-65535, "
               L"IPv4 only, IPv6 is not supported), \"pipe://NAME\", \"dmx://SERVICE\", "
               L"\"serial://COM5\" (1-255). An empty endpoint asks the facade to "
               L"resolve an in-process link.";

      case p2pf::P2PF_E_UNRESOLVED:
        return L"there is nothing to resolve an omitted endpoint to, or the peer is "
               L"unknown to this hub, or the deployment map has no entry for that "
               L"address. An omitted endpoint is looked up in the map set by "
               L"SetEndpoint/SetEndpointMap first, and failing that needs a hub of "
               L"that address ALREADY LISTENING for this one in this same process -- "
               L"so: put the address in the map, use Link to arm both ends of an "
               L"in-process edge in the right order, or spell the endpoint out. TCP "
               L"hosts, pipe names and COM ports are deployment facts and are never "
               L"guessed from an address.";

      case E_INVALIDARG:
        return L"an argument was rejected. On Listen/Connect this is almost always the "
               L"PEER slot: it may not be empty, may not equal this hub's own address, "
               L"and may not contain ':' or '//' -- that last one is a swapped-argument "
               L"call, which would otherwise arm a connection to a URI-shaped address "
               L"whose every message dies undelivered. The peer is the address of the "
               L"hub on the OTHER end; the endpoint is where to reach it.";

      case p2pf::P2PF_E_CON_DUPLICATE:
        return L"this hub already has a connection for that peer. One hub holds at most "
               L"one connection per peer address, whatever the transport; the existing "
               L"one is untouched.";

      // Shared by topics and by FIELD NAMES (facade ABI 8), which is why it
      // names the thing rather than the parameter: the rule is one rule.
      case p2pf::P2PF_E_RESERVED_TOPIC:
        return L"the topic or field name starts with \"P2Pmsg\" (the kernel's reserved "
               L"namespace) or \"P2PF\" (the facade's own, which carries its timer, "
               L"ping and post-to-pump traffic and the field index). Choose any other "
               L"prefix.";

      case p2pf::P2PF_E_CLOSED:
        return L"this hub (or the network) is already closed. The object stays alive so "
               L"the call is safe, but it does nothing; create a new hub.";

      case p2pf::P2PF_E_HUB_DUPLICATE:
        return L"a live hub of this process already answers to that address. Two hubs "
               L"sharing one address corrupt the kernel's registry, so nothing was "
               L"created. Closing the first frees the address.";

      case p2pf::P2PF_E_NO_HUB:
        return L"no live hub of this network has that address. Link joins two hubs this "
               L"network created and still holds -- both must exist before it is called, "
               L"and a hub that has been closed is gone.";

      case p2pf::P2PF_E_LINK_PARTIAL:
        return L"the dialing side failed AND the listener armed for it could not be "
               L"retracted, so the listening hub is left holding a connection for a peer "
               L"that never arrived. Close that hub to clear it.";

      case p2pf::P2PF_E_CON_FACTORY:
        return L"the transport refused the endpoint. A TCP port already in use, a pipe "
               L"name already served, a missing COM port and an absent Dmx service all "
               L"arrive here.";

      case p2pf::P2PF_E_ABI_MISMATCH:
        return L"TargetFacade.dll next to this server was built against a different ABI "
               L"version. Deploy the matching pair.";

      case p2pf::P2PF_E_STARTUP:
        return L"the messaging kernel failed to start (StartupP2Pmsg / WSAStartup).";

      case p2pf::P2PF_E_HUB_SPAWN:
        return L"the hub's pump thread failed to start.";

      // --- facade ABI 6 ---
      case p2pf::P2PF_E_NO_PEER:
        return L"this hub has no connection for that peer. Disconnect and the "
               L"connection options act on a LIVE connection, so the peer must be one "
               L"this hub armed or learned at login -- ConCount/PeerAt list exactly "
               L"those, and Description prints them.";

      case p2pf::P2PF_E_TIMEOUT:
        return L"no answer inside the budget. Ping is answered by the TargetFacade at "
               L"the far end, so a peer that is not one -- or is not up, or is not "
               L"reachable by routing -- simply never replies. Check IsPeerUp first.";

      case p2pf::P2PF_E_NO_SINK:
        return L"nothing is registered to receive it. A timer fires as an OnTimer "
               L"event, so a client with no sink attached to the hub's connection "
               L"point would never see it; attach one before arming timers.";

      case p2pf::P2PF_E_OPTION:
        return L"unknown connection option, or a write to a read-only one. See "
               L"P2PConOption: Trace, MaxSend and MaxRecv can be set; Encrypted, "
               L"ConState and Mode can only be read.";

      case p2pf::P2PF_E_PUMP_THREAD:
        return L"this call waits for the hub's own thread, so it cannot be made FROM "
               L"it. Disconnect and Ping must not be called from inside an event "
               L"handler that the hub itself is running.";

      case p2pf::P2PF_E_NO_MESSAGE:
        return L"MsgDest, MsgPriority, MsgTag and MsgFlags describe the message whose "
               L"OnMessage event is being raised, so they can only be read from inside "
               L"an OnMessage handler. Outside one there is no message to describe, and "
               L"answering 0 would be indistinguishable from a sender that set no tag.";

      case p2pf::P2PF_E_NO_FIELD:
        return L"this message carries no field of that name. A field that is PRESENT "
               L"but empty is a different thing and answers an empty array -- use "
               L"MsgFieldCount and MsgFieldName to see what a message actually has.";

      case p2pf::P2PF_E_FIELD_LIMIT:
        return L"too many fields, a field name that is too long, or a value too large. "
               L"One message may carry 64 fields, a name of 63 characters and a value "
               L"of 8192 bytes; the payload and every value together must still fit "
               L"MaxPayload.";

      case DISP_E_TYPEMISMATCH:
        return L"the payload must be a byte array (SAFEARRAY of VT_UI1) or a string. A "
               L"number, an object or an array of anything else cannot be put on the "
               L"wire; use SendText for text.";

      case E_OUTOFMEMORY:
        return L"out of memory.";

      case E_POINTER:
        return L"a required out-parameter was NULL.";

      default:
        break;
    }

    if ( hr == HRESULT_FROM_WIN32 ( ERROR_MORE_DATA ) )
        return L"the value kept growing between the sizing pass and the reading one -- "
               L"peers were logging in throughout. Ask again.";

    return NULL;                        // not ours: leave the code to speak for itself
}

// Named, echoed, explained.  Echoing the ARGUMENTS is half the value: it is what
// turns "0x80040208" into "Listen ( 'Demo.Client', 'tpc://:7788' )", and the typo
// is then visible without a debugger.
HRESULT ComFail ( const IID& iid, HRESULT hr, LPCWSTR wszCall
                , BSTR bsArg1, BSTR bsArg2, BSTR bsArg3 )
{
    LPCWSTR wszWhy = MeaningOf ( hr );
    if ( wszWhy == NULL )
        return hr;                      // nothing to add; do not overwrite a better one

    ATL::CComBSTR bs ( wszCall );
    bs += L" ( ";
    if ( bsArg1 != NULL ) { bs += L"'"; bs += bsArg1; bs += L"'"; }
    if ( bsArg2 != NULL ) { bs += L", '"; bs += bsArg2; bs += L"'"; }
    if ( bsArg3 != NULL ) { bs += L", '"; bs += bsArg3; bs += L"'"; }
    bs += L" ) failed: ";
    bs += wszWhy;

    // CLSID_P2PNetwork for both objects on purpose -- it is the one coclass with
    // a ProgID, so Err.Source resolves to "TargetCom.P2PNetwork" rather than
    // being left empty (P2PHub is noncreatable and has none).  Which OBJECT
    // failed is already in the description, which names the interface method.
    return ATL::AtlReportError ( CLSID_P2PNetwork, (LPCOLESTR)bs, iid, hr );
}

// Every IP2PHubCom entry point below funnels its failures through this.
static inline HRESULT Fail ( HRESULT hr, LPCWSTR wszCall
                           , BSTR bsArg1 = NULL, BSTR bsArg2 = NULL )
{
    return ComFail ( IID_IP2PHubCom, hr, wszCall, bsArg1, bsArg2 );
}

// ---------------------------------------------------------------------------
// construction / teardown
// ---------------------------------------------------------------------------
CP2PHubCom::CP2PHubCom ( )
    : m_pHub ( NULL ), m_pOwner ( NULL )
    , m_hThread ( NULL ), m_hWake ( NULL ), m_uThreadId ( 0 )
    , m_lQuit ( 0 ), m_lHubClosed ( 0 )
    , m_cDropped ( 0 ), m_dwNextCookie ( 1 )
    , m_pCurEvent ( NULL ), m_guidMsgCausality ( GUID_NULL ), m_lMsgActive ( 0 )
{
    m_sink.owner = this;
}

HRESULT CP2PHubCom::Init ( p2pf::IP2PNetwork *pNet, CP2PNetworkCom *pOwner, LPCWSTR wszAddress )
{
    m_pOwner = pOwner;

    m_hWake = ::CreateEvent ( NULL, FALSE, FALSE, NULL );
    if ( m_hWake == NULL )
        return HRESULT_FROM_WIN32 ( ::GetLastError() );

    // The dispatch thread holds a reference on this object, so a client that
    // drops its last reference without calling Close() cannot pull the object
    // out from under a running handler.  The network's own reference is what
    // finally releases it (see CP2PNetworkCom::FinalRelease).
    GetUnknown()->AddRef();

    m_hThread = (HANDLE)::_beginthreadex ( NULL, 0, DispatchThunk, this, 0, &m_uThreadId );
    if ( m_hThread == NULL )
    {
        GetUnknown()->Release();
        ::CloseHandle ( m_hWake );
        m_hWake = NULL;
        return HRESULT_FROM_WIN32 ( ERROR_NOT_ENOUGH_MEMORY );
    }

    // Started last: from here on callbacks may arrive, and the queue + thread
    // are already able to take them.
    HRESULT hr = pNet->CreateHub ( wszAddress, &m_sink, &m_pHub );
    if ( SUCCEEDED(hr) )
    {
        // One object, both interfaces (facade ABI 6). This is what brings
        // OnTimer and OnEvent to this tier; from here on the facade delivers
        // OnMessageEx/OnEvent in place of OnMessage/OnError, and Sink turns
        // both back into the queued events this layer already had.
        m_pHub->SetExtEvents ( &m_sink );
    }
    if ( FAILED(hr) )
    {
        ::InterlockedExchange ( &m_lQuit, 1 );
        ::SetEvent ( m_hWake );
        ::WaitForSingleObject ( m_hThread, kCloseJoinMs );
        return hr;
    }
    return S_OK;
}

void CP2PHubCom::FinalRelease ( )
{
    Close();                                   // idempotent

    if ( m_hThread != NULL ) { ::CloseHandle ( m_hThread ); m_hThread = NULL; }
    if ( m_hWake   != NULL ) { ::CloseHandle ( m_hWake );   m_hWake   = NULL; }

    IGlobalInterfaceTable *pGit = GetGit();
    m_csSinks.Lock();
    if ( pGit != NULL )
        for ( std::map<DWORD,DWORD>::iterator it = m_sinks.begin(); it != m_sinks.end(); ++it )
            pGit->RevokeInterfaceFromGlobal ( it->second );
    m_sinks.clear();
    m_csSinks.Unlock();
}

void CP2PHubCom::CloseKernelHub ( )
{
    if ( ::InterlockedExchange ( &m_lHubClosed, 1 ) != 0 )
        return;                                 // someone else already claimed it

    p2pf::IP2PHub *pHub = (p2pf::IP2PHub*)::InterlockedExchangePointer ( (PVOID*)&m_pHub, NULL );
    if ( pHub != NULL )
        pHub->Close();
}

HRESULT CP2PHubCom::Live ( p2pf::IP2PHub **ppHub ) const
{
    p2pf::IP2PHub *pHub = m_pHub;
    if ( pHub == NULL ) return p2pf::P2PF_E_CLOSED;
    *ppHub = pHub;
    return S_OK;
}

// ---------------------------------------------------------------------------
// the pump-thread sink: copy and enqueue, nothing else
// ---------------------------------------------------------------------------
void CP2PHubCom::Sink::OnMessage ( const wchar_t *source, const wchar_t *topic
                                 , const void *payload, unsigned int size
                                 , bool broadcast )
{
    HubEvent ev;
    ev.kind      = HubEvent::evMessage;
    ev.a         = ( source != NULL ) ? source : L"";
    ev.b         = ( topic  != NULL ) ? topic  : L"";
    ev.broadcast = broadcast ? VARIANT_TRUE : VARIANT_FALSE;
    if ( payload != NULL && size != 0 )
    {
        const BYTE *p = (const BYTE*)payload;
        ev.payload.assign ( p, p + size );      // the pointer dies with this call
    }

    // The ABI 7 properties, captured HERE (facade ABI 7).
    //
    // This is the only moment they can be had: GetMsgInfo is defined inside a
    // delivery on the pump thread, and this call IS that delivery. By the time
    // the dispatch thread replays this event the kernel's message is gone, so
    // whatever is not copied now can never be answered at all.
    //
    // ONE call, into a stack buffer, because it runs on the kernel pump for
    // every message whether or not any client ever reads a tag. An address
    // longer than the buffer reports empty rather than costing a second call
    // and an allocation on that thread -- P2Paddr values are short and this is
    // the pump.
    if ( owner != NULL && owner->m_pHub != NULL )
    {
        wchar_t      wszDest[256];
        unsigned int cchDest  = (unsigned int)( sizeof(wszDest) / sizeof(wszDest[0]) );
        unsigned int uPri     = 7, uTag = 0, uFlags = 0;

        HRESULT hr = owner->m_pHub->GetMsgInfo ( wszDest, &cchDest, &uPri,
                                                 &uTag, &uFlags );
        if ( SUCCEEDED(hr) )
            ev.msgDest = wszDest;
        ev.msgPriority = (LONG)uPri;
        ev.msgTag      = (LONG)uTag;
        ev.msgFlags    = (LONG)uFlags;

        // The named fields (facade ABI 8), same moment, same reason -- and
        // gated on the flag, so a message that carries none costs the pump
        // thread one bit test rather than three calls.
        if ( ( uFlags & p2pf::P2PF_MSG_FIELDS ) != 0 )
        {
            unsigned int nFields = 0;
            owner->m_pHub->GetFieldCount ( &nFields );
            for ( unsigned int i = 0; i < nFields; ++i )
            {
                wchar_t      wszName[128];
                unsigned int cchName = (unsigned int)( sizeof(wszName) / sizeof(wszName[0]) );
                if ( FAILED ( owner->m_pHub->GetFieldName ( i, wszName, &cchName ) ) )
                    continue;

                HubEvent::QueuedField f;
                f.name = wszName;

                unsigned int cb = 0;
                if ( SUCCEEDED ( owner->m_pHub->GetField ( wszName, NULL, &cb ) ) )
                {
                    f.value.resize ( cb );
                    if ( cb && FAILED ( owner->m_pHub->GetField ( wszName, &f.value[0], &cb ) ) )
                        f.value.clear();
                }
                ev.fields.push_back ( f );
            }
        }
    }

    owner->Push ( ev );
}

void CP2PHubCom::Sink::OnPeerUp ( const wchar_t *peer )
{
    HubEvent ev; ev.kind = HubEvent::evPeerUp;
    ev.a = ( peer != NULL ) ? peer : L"";
    owner->Push ( ev );
}

void CP2PHubCom::Sink::OnPeerDown ( const wchar_t *peer )
{
    HubEvent ev; ev.kind = HubEvent::evPeerDown;
    ev.a = ( peer != NULL ) ? peer : L"";
    owner->Push ( ev );
}

void CP2PHubCom::Sink::OnError ( const wchar_t *what )
{
    HubEvent ev; ev.kind = HubEvent::evError;
    ev.a = ( what != NULL ) ? what : L"";
    owner->Push ( ev );
}

// --- facade ABI 6 ----------------------------------------------------------

HRESULT CP2PHubCom::Sink::OnMessageEx ( const wchar_t *source, const wchar_t *topic
                                      , const void *payload, unsigned int size
                                      , bool broadcast )
{
    // Same copy-and-enqueue, and S_OK is the only honest answer: nothing here
    // has asked the client anything yet, and cannot -- the sink does not run
    // until the dispatch thread replays this, which is the whole point of the
    // queue (a slow handler must never stall the kernel pump).
    OnMessage ( source, topic, payload, size, broadcast );
    return S_OK;
}

void CP2PHubCom::Sink::OnEvent ( unsigned int code, const wchar_t *peer
                               , const wchar_t *what )
{
    // TWO events, deliberately. The flat ABI substitutes OnEvent FOR OnError;
    // this tier cannot, because OnError (dispid 4) is published and a sink
    // that only handles it must keep working exactly as it did. So the legacy
    // report goes out unchanged, and the structured one follows it.
    HubEvent evLegacy; evLegacy.kind = HubEvent::evError;
    evLegacy.a = ( what != NULL ) ? what : L"";
    owner->Push ( evLegacy );

    HubEvent ev; ev.kind = HubEvent::evEvent;
    ev.n1 = (LONG)code;
    ev.a  = ( peer != NULL ) ? peer : L"";
    ev.b  = ( what != NULL ) ? what : L"";
    owner->Push ( ev );
}

void CP2PHubCom::Sink::OnTimer ( unsigned int timerId, unsigned int key )
{
    HubEvent ev; ev.kind = HubEvent::evTimer;
    ev.n1 = (LONG)timerId;
    ev.n2 = (LONG)key;
    owner->Push ( ev );
}

void CP2PHubCom::Sink::OnPost ( unsigned int, void* )
{
    // Unreachable by construction: IP2PHub::Post is the one facade method this
    // layer does not project (it carries a raw in-process pointer), so nothing
    // can ask for this callback. Present because the interface requires it.
}

// ---------------------------------------------------------------------------
// queue
// ---------------------------------------------------------------------------
void CP2PHubCom::Push ( const HubEvent& ev )
{
    bool bWake = false;

    m_csQueue.Lock();
    if ( m_queue.size() >= kMaxQueued )
        ++m_cDropped;                           // client can't keep up; say so later
    else
    {
        m_queue.push_back ( ev );
        bWake = true;
    }
    m_csQueue.Unlock();

    if ( bWake && m_hWake != NULL )
        ::SetEvent ( m_hWake );
}

bool CP2PHubCom::Pop ( HubEvent& ev )
{
    bool bGot = false;

    m_csQueue.Lock();
    if ( !m_queue.empty() )
    {
        ev = m_queue.front();
        m_queue.pop_front();
        bGot = true;
    }
    else if ( m_cDropped != 0 )
    {
        // Drained: report the gap instead of hiding it.
        WCHAR wsz[128];
        ::swprintf_s ( wsz, L"%ld event(s) dropped: the event sink could not keep up", m_cDropped );
        m_cDropped = 0;
        ev = HubEvent();
        ev.kind = HubEvent::evError;
        ev.a    = wsz;
        bGot    = true;
    }
    m_csQueue.Unlock();

    return bGot;
}

// ---------------------------------------------------------------------------
// dispatch thread
// ---------------------------------------------------------------------------
unsigned __stdcall CP2PHubCom::DispatchThunk ( void *pThis )
{
    ((CP2PHubCom*)pThis)->DispatchLoop();
    return 0;
}

void CP2PHubCom::DispatchLoop ( )
{
    ::CoInitializeEx ( NULL, COINIT_MULTITHREADED );

    for ( ;; )
    {
        ::WaitForSingleObject ( m_hWake, INFINITE );

        HubEvent ev;
        while ( Pop ( ev ) )
            Fire ( ev );                        // may block for as long as the client likes

        if ( ::InterlockedCompareExchange ( &m_lQuit, 0, 0 ) != 0 )
            break;
    }

    ::CoUninitialize();

    // Whoever calls Close() normally does the teardown; this covers the case
    // where Close() came from inside a handler (i.e. from THIS thread) and had
    // to hand the teardown over.
    CloseKernelHub();

    IUnknown *pUnk = GetUnknown();
    pUnk->Release();                            // may destroy `this` -- last statement
}

void CP2PHubCom::Fire ( const HubEvent& ev )
{
    std::vector<DWORD> git;

    m_csSinks.Lock();
    for ( std::map<DWORD,DWORD>::iterator it = m_sinks.begin(); it != m_sinks.end(); ++it )
        git.push_back ( it->second );
    m_csSinks.Unlock();

    if ( git.empty() ) return;

    IGlobalInterfaceTable *pGit = GetGit();
    if ( pGit == NULL ) return;

    // DISPPARAMS carries arguments in REVERSE declaration order.
    ATL::CComVariant args[4];
    DISPID           dispid = 0;
    UINT             cArgs  = 0;

    switch ( ev.kind )
    {
      case HubEvent::evMessage:
      {
        VARIANT vPayload;
        if ( FAILED ( BytesToVariant ( ev.payload.empty() ? NULL : &ev.payload[0]
                                     , (unsigned int)ev.payload.size(), &vPayload ) ) )
            return;
        dispid  = 1;
        args[0] = ATL::CComVariant ( ev.broadcast ? true : false );
        args[1].Attach ( &vPayload );
        args[2] = ATL::CComVariant ( ev.b );
        args[3] = ATL::CComVariant ( ev.a );
        cArgs   = 4;
        break;
      }
      case HubEvent::evPeerUp:   dispid = 2; args[0] = ATL::CComVariant ( ev.a ); cArgs = 1; break;
      case HubEvent::evPeerDown: dispid = 3; args[0] = ATL::CComVariant ( ev.a ); cArgs = 1; break;
      case HubEvent::evError:    dispid = 4; args[0] = ATL::CComVariant ( ev.a ); cArgs = 1; break;

      // Reverse declaration order, like evMessage above.
      case HubEvent::evEvent:
        dispid  = 5;
        args[0] = ATL::CComVariant ( ev.b );        // what
        args[1] = ATL::CComVariant ( ev.a );        // peer
        args[2] = ATL::CComVariant ( ev.n1 );       // code
        cArgs   = 3;
        break;

      case HubEvent::evTimer:
        dispid  = 6;
        args[0] = ATL::CComVariant ( ev.n2 );       // key
        args[1] = ATL::CComVariant ( ev.n1 );       // timerId
        cArgs   = 2;
        break;

      default: return;
    }

    DISPPARAMS dp;
    ::ZeroMemory ( &dp, sizeof(dp) );
    dp.rgvarg  = args;
    dp.cArgs   = cArgs;

    // Publish the message for the MsgTag/MsgDest/... properties, for the length
    // of this event and no longer (facade ABI 7).  `ev` outlives the loop --
    // DispatchLoop owns it -- so the pointer is good for every sink.
    if ( ev.kind == HubEvent::evMessage )
    {
        m_pCurEvent = &ev;
        if ( FAILED ( ::CoGetCurrentLogicalThreadId ( &m_guidMsgCausality ) ) )
            m_guidMsgCausality = GUID_NULL;
        ::InterlockedExchange ( &m_lMsgActive, 1 );
    }

    for ( size_t i = 0; i < git.size(); ++i )
    {
        ATL::CComPtr<IDispatch> spSink;
        // Re-fetching per fire is what produces a proxy valid in THIS
        // apartment; a cached raw pointer would be an illegal cross-apartment
        // call for every STA client.
        if ( FAILED ( pGit->GetInterfaceFromGlobal ( git[i], IID_IDispatch, (void**)&spSink ) ) )
            continue;
        if ( spSink == NULL )
            continue;

        spSink->Invoke ( dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD
                       , &dp, NULL, NULL, NULL );   // a sink's failure is its own business
    }

    if ( ev.kind == HubEvent::evMessage )
    {
        ::InterlockedExchange ( &m_lMsgActive, 0 );
        m_pCurEvent = NULL;
    }
}

// ---------------------------------------------------------------------------
// The message model -- the four read properties               (facade ABI 7)
// ---------------------------------------------------------------------------
//
// The gate answers one question -- "is this read part of the call chain that
// is raising a message event?" -- and answers p2pfNoMessage to everything
// else rather than a stale value, because "the sender set no tag" and "you
// asked in the wrong place" must not both read as 0.
//
// Causality, not thread: see the m_guidMsgCausality note in ComHub.h.  A
// handler in an STA reading MsgTag produces an incoming call on an RPC pool
// thread, which shares nothing with the dispatch thread except this id.
//
HRESULT CP2PHubCom::CurEvent ( const HubEvent **ppEv ) const
{
    if ( ::InterlockedCompareExchange (
                    const_cast<volatile LONG*>(&m_lMsgActive), 0, 0 ) == 0 ||
         m_pCurEvent == NULL )
      return ComFail ( IID_IP2PHubCom, p2pf::P2PF_E_NO_MESSAGE, L"Msg*" );

    GUID guid;
    if ( FAILED ( ::CoGetCurrentLogicalThreadId ( &guid ) ) ||
         !::IsEqualGUID ( guid, m_guidMsgCausality ) )
      return ComFail ( IID_IP2PHubCom, p2pf::P2PF_E_NO_MESSAGE, L"Msg*" );

    *ppEv = m_pCurEvent;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::get_MsgDest ( BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;
    *pVal = ::SysAllocString ( pEv->msgDest ? (BSTR)pEv->msgDest : L"" );
    return ( *pVal != NULL ) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CP2PHubCom::get_MsgPriority ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = 0;
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;
    *pVal = pEv->msgPriority;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::get_MsgTag ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = 0;
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;
    *pVal = pEv->msgTag;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::get_MsgFlags ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = 0;
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;
    *pVal = pEv->msgFlags;
    return S_OK;
}

// --- named fields (facade ABI 8) -------------------------------------------

STDMETHODIMP CP2PHubCom::get_MsgFieldCount ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = 0;
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;
    *pVal = (LONG)pEv->fields.size();
    return S_OK;
}

STDMETHODIMP CP2PHubCom::MsgFieldName ( LONG index, BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;

    if ( index < 0 || (size_t)index >= pEv->fields.size() )
        return Fail ( E_INVALIDARG, L"MsgFieldName" );

    *pVal = ::SysAllocString ( pEv->fields[(size_t)index].name );
    return ( *pVal != NULL ) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CP2PHubCom::MsgField ( BSTR name, VARIANT *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    ::VariantInit ( pVal );
    const HubEvent *pEv; HRESULT hr = CurEvent ( &pEv );
    if ( FAILED(hr) ) return hr;

    for ( size_t i = 0; i < pEv->fields.size(); ++i )
    {
        if ( ::wcscmp ( Str(pEv->fields[i].name), Str(name) ) != 0 )
            continue;
        // Present-but-empty answers an EMPTY ARRAY; absent raises p2pfNoField
        // below. Keeping the two apart is the point of the distinction.
        const std::vector<BYTE>& v = pEv->fields[i].value;
        return BytesToVariant ( v.empty() ? NULL : &v[0]
                              , (unsigned int)v.size(), pVal );
    }
    return Fail ( p2pf::P2PF_E_NO_FIELD, L"MsgField", name );
}

// ---------------------------------------------------------------------------
// IP2PHubCom -- every method is a thin, validated forward to p2pf::IP2PHub
// ---------------------------------------------------------------------------
// A NULL/empty `endpoint` is meaningful -- it asks the facade to resolve an
// in-process link -- so it is passed straight through rather than guarded, and
// Str() turning NULL into L"" is exactly the right coercion for a script that
// omits the argument.
STDMETHODIMP CP2PHubCom::Listen ( BSTR toPeer, BSTR endpoint )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( SUCCEEDED(hr) ) hr = pHub->Listen ( Str(toPeer), Str(endpoint) );

    // FAILED, never !S_OK: P2PF_S_UNRELATED_LINK is a SUCCESS code and has to
    // reach a vtable caller exactly as the facade produced it.
    return FAILED(hr) ? Fail ( hr, L"Listen", toPeer, endpoint ) : hr;
}

STDMETHODIMP CP2PHubCom::Connect ( BSTR toPeer, BSTR endpoint )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( SUCCEEDED(hr) ) hr = pHub->Connect ( Str(toPeer), Str(endpoint) );
    return FAILED(hr) ? Fail ( hr, L"Connect", toPeer, endpoint ) : hr;
}

STDMETHODIMP CP2PHubCom::Send ( BSTR dest, BSTR topic, VARIANT payload )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );

    std::vector<BYTE> bytes;
    if ( SUCCEEDED(hr) ) hr = VariantToBytes ( payload, bytes );
    if ( SUCCEEDED(hr) )
        hr = pHub->Send ( Str(dest), Str(topic)
                        , bytes.empty() ? NULL : &bytes[0], (unsigned int)bytes.size() );

    return FAILED(hr) ? Fail ( hr, L"Send", dest, topic ) : hr;
}

STDMETHODIMP CP2PHubCom::SendText ( BSTR dest, BSTR topic, BSTR text )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( SUCCEEDED(hr) ) hr = pHub->SendText ( Str(dest), Str(topic), Str(text) );
    return FAILED(hr) ? Fail ( hr, L"SendText", dest, topic ) : hr;
}

STDMETHODIMP CP2PHubCom::Broadcast ( BSTR topic, VARIANT payload, VARIANT_BOOL *pDelivered )
{
    if ( pDelivered == NULL ) return E_POINTER;
    *pDelivered = VARIANT_FALSE;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );

    std::vector<BYTE> bytes;
    if ( SUCCEEDED(hr) ) hr = VariantToBytes ( payload, bytes );
    if ( SUCCEEDED(hr) )
        hr = pHub->Broadcast ( Str(topic)
                             , bytes.empty() ? NULL : &bytes[0], (unsigned int)bytes.size() );
    if ( FAILED(hr) ) return Fail ( hr, L"Broadcast", topic );

    // S_FALSE ("no peer was up") is invisible to an automation client, so it
    // comes back as the return value instead.
    *pDelivered = ( hr == S_OK ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

// --- the message model (facade ABI 7) --------------------------------------
//
// Sign matters exactly once here. `tag` is a LONG on this side and an unsigned
// int on the flat one, so the cast is a bit-for-bit reinterpretation and a
// script that writes &H80000001 reads the same bits back as a negative LONG.
// `priority` is not the same case: -1 is the ONE negative value that means
// anything (p2pfPriDefault, "leave it alone"), and it maps onto the flat
// P2PF_PRI_DEFAULT, which is 0xFFFFFFFF -- the same bits, again.

STDMETHODIMP CP2PHubCom::SendEx ( BSTR dest, BSTR topic, VARIANT payload,
                                  LONG tag, LONG priority, LONG flags )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );

    std::vector<BYTE> bytes;
    if ( SUCCEEDED(hr) ) hr = VariantToBytes ( payload, bytes );
    if ( SUCCEEDED(hr) )
        hr = pHub->SendEx ( Str(dest), Str(topic)
                          , bytes.empty() ? NULL : &bytes[0], (unsigned int)bytes.size()
                          , (unsigned int)priority, (unsigned int)tag
                          , (unsigned int)flags );

    return FAILED(hr) ? Fail ( hr, L"SendEx", dest, topic ) : hr;
}

STDMETHODIMP CP2PHubCom::BroadcastEx ( BSTR topic, VARIANT payload,
                                       LONG tag, LONG priority, LONG flags,
                                       VARIANT_BOOL *pDelivered )
{
    if ( pDelivered == NULL ) return E_POINTER;
    *pDelivered = VARIANT_FALSE;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );

    std::vector<BYTE> bytes;
    if ( SUCCEEDED(hr) ) hr = VariantToBytes ( payload, bytes );
    if ( SUCCEEDED(hr) )
        hr = pHub->BroadcastEx ( Str(topic)
                               , bytes.empty() ? NULL : &bytes[0], (unsigned int)bytes.size()
                               , (unsigned int)priority, (unsigned int)tag
                               , (unsigned int)flags );
    if ( FAILED(hr) ) return Fail ( hr, L"BroadcastEx", topic );

    *pDelivered = ( hr == S_OK ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

// --- sending a message object (facade ABI 8) -------------------------------
//
// The IP2PMessageCom handed in must be one of OURS -- it is unwrapped to the
// flat p2pf::IP2PMessage underneath, which is what the facade's SendMsg takes.
// A foreign implementation of the interface would QueryInterface successfully
// and then have no flat object behind it, so the cast is guarded by the QI for
// our own IID rather than assumed.
static HRESULT FlatOf ( IP2PMessageCom *pMsg, p2pf::IP2PMessage **ppFlat )
{
    if ( pMsg == NULL || ppFlat == NULL ) return E_POINTER;
    *ppFlat = NULL;

    ATL::CComPtr<IP2PMessageCom> spMine;
    if ( FAILED ( pMsg->QueryInterface ( IID_IP2PMessageCom, (void**)&spMine ) ) )
        return E_INVALIDARG;

    CP2PMessageCom *pObj = static_cast<CP2PMessageCom*>( spMine.p );
    *ppFlat = pObj->Flat();
    return ( *ppFlat != NULL ) ? S_OK : E_UNEXPECTED;
}

STDMETHODIMP CP2PHubCom::SendMsg ( BSTR dest, BSTR topic, IP2PMessageCom *msg,
                                   LONG tag, LONG priority, LONG flags )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );

    p2pf::IP2PMessage *pFlat = NULL;
    if ( SUCCEEDED(hr) ) hr = FlatOf ( msg, &pFlat );
    if ( SUCCEEDED(hr) )
        hr = pHub->SendMsg ( Str(dest), Str(topic), pFlat
                           , (unsigned int)priority, (unsigned int)tag
                           , (unsigned int)flags );

    return FAILED(hr) ? Fail ( hr, L"SendMsg", dest, topic ) : hr;
}

STDMETHODIMP CP2PHubCom::BroadcastMsg ( BSTR topic, IP2PMessageCom *msg,
                                        LONG tag, LONG priority, LONG flags,
                                        VARIANT_BOOL *pDelivered )
{
    if ( pDelivered == NULL ) return E_POINTER;
    *pDelivered = VARIANT_FALSE;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );

    p2pf::IP2PMessage *pFlat = NULL;
    if ( SUCCEEDED(hr) ) hr = FlatOf ( msg, &pFlat );
    if ( SUCCEEDED(hr) )
        hr = pHub->BroadcastMsg ( Str(topic), pFlat
                                , (unsigned int)priority, (unsigned int)tag
                                , (unsigned int)flags );
    if ( FAILED(hr) ) return Fail ( hr, L"BroadcastMsg", topic );

    *pDelivered = ( hr == S_OK ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::get_Address ( BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"Address" );

    const wchar_t *wsz = pHub->Address();
    *pVal = ::SysAllocString ( ( wsz != NULL ) ? wsz : L"" );
    return ( *pVal != NULL ) ? S_OK : Fail ( E_OUTOFMEMORY, L"Address" );
}

STDMETHODIMP CP2PHubCom::IsPeerUp ( BSTR peer, VARIANT_BOOL *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = VARIANT_FALSE;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"IsPeerUp", peer );

    *pVal = pHub->IsPeerUp ( Str(peer) ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::Close ( )
{
    // No self-AddRef here on purpose: this also runs from FinalRelease, where
    // the reference count is already 0 and an AddRef/Release pair would delete
    // the object a second time.  It is safe without one -- a client calling
    // Close() necessarily holds a reference, and by the time FinalRelease can
    // run, m_pOwner is already NULL so nothing is released below.
    if ( ::InterlockedExchange ( &m_lQuit, 1 ) == 0 && m_hWake != NULL )
        ::SetEvent ( m_hWake );

    if ( m_hThread != NULL && ::GetCurrentThreadId() != (DWORD)m_uThreadId )
    {
        // A timeout here means the client called Close() from inside its own
        // handler on another thread; the dispatch thread finishes the teardown
        // when that handler returns (see DispatchLoop).
        ::WaitForSingleObject ( m_hThread, kCloseJoinMs );
        CloseKernelHub();
    }
    else if ( m_hThread == NULL )
    {
        CloseKernelHub();                       // never started
    }
    // else: called ON the dispatch thread -- DispatchLoop closes on its way out.

    if ( m_pOwner != NULL )
    {
        CP2PNetworkCom *pOwner = m_pOwner;
        m_pOwner = NULL;
        pOwner->ForgetHub ( this );             // drops the network's reference
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// IP2PHubCom -- the read side (dispids 9-13)
//
// Everything below already exists on p2pf::IP2PHub.  What is added here is the
// ADAPTATION: the flat side speaks a caller-sized buffer protocol (ask for the
// size with a NULL buffer, allocate, ask again), which is right for C++ and is
// exactly the shape a scripting host handles worst.  So the buffer dance
// happens once, here, and every member hands back one BSTR or one LONG.
//
// Why the read side is on the COM interface at all: Listen/Connect can return
// P2PF_S_UNRELATED_LINK, a SUCCESS code, and automation discards success codes
// -- ITypeInfo::Invoke normalises them to S_OK and the CLR marshaller has
// nowhere to put them.  A C++ vtable caller sees it; VB, VBScript, C# and
// IDispatch do not.  These members let all of them ask afterwards instead.
// ---------------------------------------------------------------------------

// The three string getters differ only in their extra argument, so they are
// selected rather than duplicated.
enum SizedRead { kReadDescribe, kReadEndpoint, kReadPeerAt };

static HRESULT ReadOne ( p2pf::IP2PHub *pHub, SizedRead eKind
                       , LPCWSTR wszPeer, unsigned int uIndex
                       , wchar_t *buf, unsigned int *cch )
{
    switch ( eKind )
    {
      case kReadDescribe: return pHub->Describe    ( buf, cch );
      case kReadEndpoint: return pHub->GetEndpoint ( wszPeer, buf, cch );
      case kReadPeerAt:   return pHub->GetCon      ( uIndex, buf, cch, NULL, NULL, NULL );
    }
    return E_UNEXPECTED;
}

// The whole caller-sized buffer protocol, run once here so no client ever has
// to: ask for the size with a NULL buffer, allocate, ask again.  *cch counts
// the terminator; a BSTR's length does NOT, and SysAllocStringLen allocates one
// character more than it is asked for -- so cch-1 is exactly right and the
// facade's wcscpy_s into it stays in bounds.
//
// The retry is not defensive padding.  The set these getters read is LIVE: a
// login on a pump thread between the sizing pass and the filling one makes the
// value longer, and the second pass then comes back ERROR_MORE_DATA.  A COM
// client has no way to react to that, so it is absorbed here; only a value that
// keeps growing through every attempt escapes.
static HRESULT BstrFromRead ( p2pf::IP2PHub *pHub, SizedRead eKind
                            , LPCWSTR wszPeer, unsigned int uIndex, BSTR *pVal )
{
    static const int kTries = 4;

    for ( int i = 0; i < kTries; ++i )
    {
        unsigned int cch = 0;
        HRESULT hr = ReadOne ( pHub, eKind, wszPeer, uIndex, NULL, &cch );
        if ( FAILED(hr) ) return hr;
        if ( cch == 0 )   return E_UNEXPECTED;  // the protocol always counts the terminator

        ATL::CComBSTR bs;
        bs.Attach ( ::SysAllocStringLen ( NULL, cch - 1 ) );
        if ( !bs ) return E_OUTOFMEMORY;

        hr = ReadOne ( pHub, eKind, wszPeer, uIndex, bs, &cch );
        if ( SUCCEEDED(hr) ) { *pVal = bs.Detach(); return S_OK; }
        if ( hr != HRESULT_FROM_WIN32 ( ERROR_MORE_DATA ) ) return hr;
    }
    return HRESULT_FROM_WIN32 ( ERROR_MORE_DATA );
}

STDMETHODIMP CP2PHubCom::RelationTo ( BSTR peer, LONG *pFlags )
{
    if ( pFlags == NULL ) return E_POINTER;
    *pFlags = 0;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"RelationTo", peer );

    LPCWSTR wszPeer = Str(peer);

    // The flat ABI has no "flags for this peer" call -- GetCon is by position
    // -- so this walks the momentary ordering and matches by name.  The set is
    // live: a login on a pump thread can add a peer mid-walk, and a peer can
    // go away, which is why a short read here ends the walk rather than
    // failing it.
    unsigned int cCon = 0;
    hr = pHub->GetConCount ( &cCon );
    if ( FAILED(hr) ) return Fail ( hr, L"RelationTo", peer );

    for ( unsigned int i = 0; i < cCon; ++i )
    {
        unsigned int cch = 0, uFlags = 0;
        if ( FAILED ( pHub->GetCon ( i, NULL, &cch, NULL, NULL, &uFlags ) ) )
            break;                              // past the end: the set shrank
        if ( cch == 0 ) continue;

        std::vector<wchar_t> buf ( cch );
        unsigned int cchHave = cch;
        if ( FAILED ( pHub->GetCon ( i, &buf[0], &cchHave, NULL, NULL, &uFlags ) ) )
            continue;                           // this row changed under the walk

        if ( ::wcscmp ( &buf[0], wszPeer ) == 0 )
        {
            *pFlags = (LONG)uFlags;
            return S_OK;
        }
    }

    // Same answer GetEndpoint gives for a name this hub has never heard of.
    return Fail ( p2pf::P2PF_E_UNRESOLVED, L"RelationTo", peer );
}

STDMETHODIMP CP2PHubCom::get_ConCount ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = 0;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"ConCount" );

    unsigned int cCon = 0;
    hr = pHub->GetConCount ( &cCon );
    if ( FAILED(hr) ) return Fail ( hr, L"ConCount" );

    *pVal = (LONG)cCon;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::PeerAt ( LONG index, BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;
    if ( index < 0 ) return Fail ( E_INVALIDARG, L"PeerAt" );  // flat side is unsigned

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"PeerAt" );

    // E_INVALIDARG past the end -- the flat side's answer, passed through.
    hr = BstrFromRead ( pHub, kReadPeerAt, NULL, (unsigned int)index, pVal );
    return FAILED(hr) ? Fail ( hr, L"PeerAt" ) : hr;
}

STDMETHODIMP CP2PHubCom::EndpointFor ( BSTR peer, BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"EndpointFor", peer );

    // An empty string (with S_OK) for a peer this hub knows but never armed --
    // a wildcard listener's adopted name -- and P2PF_E_UNRESOLVED for one it
    // has never heard of.  Neither is invented here; both come from the facade.
    hr = BstrFromRead ( pHub, kReadEndpoint, Str(peer), 0, pVal );
    return FAILED(hr) ? Fail ( hr, L"EndpointFor", peer ) : hr;
}

STDMETHODIMP CP2PHubCom::get_Description ( BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"Description" );

    // One atomic snapshot, taken under the facade's own lock -- which is the
    // reason to prefer it to a ConCount/PeerAt loop for anything that has to
    // be self-consistent, a log line or a bug report included.
    hr = BstrFromRead ( pHub, kReadDescribe, NULL, 0, pVal );
    return FAILED(hr) ? Fail ( hr, L"Description" ) : hr;
}

// ---------------------------------------------------------------------------
// Past the messaging slice -- facade ABI 6, dispids 14-20
//
// Seven straight forwards.  There is nothing to reshape here the way the read
// side had to be reshaped: every parameter is already a BSTR or a LONG, which
// is what made these the cheap half of the facade's own additions.  The only
// judgement calls are which THREE do not come across at all (Post, GetNative
// and OnMessageEx's answer -- see the IDL) and the two defaults below.
// ---------------------------------------------------------------------------
STDMETHODIMP CP2PHubCom::Disconnect ( BSTR peer )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"Disconnect", peer );

    hr = pHub->Disconnect ( Str(peer) );
    return FAILED(hr) ? Fail ( hr, L"Disconnect", peer ) : hr;
}

STDMETHODIMP CP2PHubCom::SetTimer ( LONG delayMillisecs, LONG key, LONG *pTimerId )
{
    if ( pTimerId == NULL ) return E_POINTER;
    *pTimerId = 0;
    // The flat side takes unsigned; a negative delay is a caller mistake, not
    // a four-billion-millisecond timer.
    if ( delayMillisecs < 0 || key < 0 ) return Fail ( E_INVALIDARG, L"SetTimer" );

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"SetTimer" );

    unsigned int uId = 0;
    hr = pHub->SetTimer ( (unsigned int)delayMillisecs, (unsigned int)key, &uId );
    if ( FAILED(hr) ) return Fail ( hr, L"SetTimer" );

    *pTimerId = (LONG)uId;
    return hr;
}

STDMETHODIMP CP2PHubCom::KillTimer ( LONG timerId )
{
    if ( timerId < 0 ) return Fail ( E_INVALIDARG, L"KillTimer" );

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"KillTimer" );

    hr = pHub->KillTimer ( (unsigned int)timerId );
    return FAILED(hr) ? Fail ( hr, L"KillTimer" ) : hr;
}

STDMETHODIMP CP2PHubCom::Ping ( BSTR peer, LONG timeoutMillisecs, LONG *pMillisecs )
{
    if ( pMillisecs == NULL ) return E_POINTER;
    *pMillisecs = 0;
    if ( timeoutMillisecs < 0 ) return Fail ( E_INVALIDARG, L"Ping", peer );

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"Ping", peer );

    // 0 is the IDL default and means "the facade's own budget" (5 s), which is
    // what makes `hub.Ping "Peer"` a legal one-argument call from a script.
    unsigned int uMillisecs = 0;
    hr = pHub->Ping ( Str(peer), (unsigned int)timeoutMillisecs, &uMillisecs );
    if ( FAILED(hr) ) return Fail ( hr, L"Ping", peer );

    *pMillisecs = (LONG)uMillisecs;
    return hr;
}

STDMETHODIMP CP2PHubCom::SetConOption ( BSTR peer, LONG option, LONG value )
{
    if ( option < 0 || value < 0 ) return Fail ( E_INVALIDARG, L"SetConOption", peer );

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"SetConOption", peer );

    hr = pHub->SetConOption ( Str(peer), (unsigned int)option, (unsigned int)value );
    return FAILED(hr) ? Fail ( hr, L"SetConOption", peer ) : hr;
}

STDMETHODIMP CP2PHubCom::GetConOption ( BSTR peer, LONG option, LONG *pValue )
{
    if ( pValue == NULL ) return E_POINTER;
    *pValue = 0;
    if ( option < 0 ) return Fail ( E_INVALIDARG, L"GetConOption", peer );

    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"GetConOption", peer );

    unsigned int uValue = 0;
    hr = pHub->GetConOption ( Str(peer), (unsigned int)option, &uValue );
    if ( FAILED(hr) ) return Fail ( hr, L"GetConOption", peer );

    *pValue = (LONG)uValue;
    return hr;
}

STDMETHODIMP CP2PHubCom::CloseIdleCons ( )
{
    p2pf::IP2PHub *pHub; HRESULT hr = Live ( &pHub );
    if ( FAILED(hr) ) return Fail ( hr, L"CloseIdleCons" );

    hr = pHub->CloseIdleCons();
    return FAILED(hr) ? Fail ( hr, L"CloseIdleCons" ) : hr;
}

// ---------------------------------------------------------------------------
// IConnectionPointContainer / IConnectionPoint
//
// There is exactly one connection point, so this object IS its own connection
// point.  Sinks live in the GIT rather than as raw pointers -- see the header.
// ---------------------------------------------------------------------------
STDMETHODIMP CP2PHubCom::EnumConnectionPoints ( IEnumConnectionPoints **ppEnum )
{
    if ( ppEnum == NULL ) return E_POINTER;
    *ppEnum = NULL;
    return E_NOTIMPL;      // FindConnectionPoint is what every real client uses
}

STDMETHODIMP CP2PHubCom::FindConnectionPoint ( REFIID riid, IConnectionPoint **ppCP )
{
    if ( ppCP == NULL ) return E_POINTER;
    *ppCP = NULL;
    if ( !::InlineIsEqualGUID ( riid, DIID__IP2PHubEvents ) )
        return CONNECT_E_NOCONNECTION;
    return GetUnknown()->QueryInterface ( IID_IConnectionPoint, (void**)ppCP );
}

STDMETHODIMP CP2PHubCom::GetConnectionInterface ( IID *pIID )
{
    if ( pIID == NULL ) return E_POINTER;
    *pIID = DIID__IP2PHubEvents;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::GetConnectionPointContainer ( IConnectionPointContainer **ppCPC )
{
    if ( ppCPC == NULL ) return E_POINTER;
    return GetUnknown()->QueryInterface ( IID_IConnectionPointContainer, (void**)ppCPC );
}

STDMETHODIMP CP2PHubCom::Advise ( IUnknown *pUnkSink, DWORD *pdwCookie )
{
    if ( pdwCookie == NULL ) return E_POINTER;
    *pdwCookie = 0;
    if ( pUnkSink == NULL ) return E_POINTER;

    // Registered in the GIT as IID_IDispatch below, so ask for that first; a
    // dispinterface sink answers both.
    ATL::CComPtr<IDispatch> spSink;
    if ( FAILED ( pUnkSink->QueryInterface ( IID_IDispatch,       (void**)&spSink ) ) &&
         FAILED ( pUnkSink->QueryInterface ( DIID__IP2PHubEvents, (void**)&spSink ) ) )
        return CONNECT_E_CANNOTCONNECT;

    IGlobalInterfaceTable *pGit = GetGit();
    if ( pGit == NULL ) return E_UNEXPECTED;

    DWORD dwGit = 0;
    HRESULT hr = pGit->RegisterInterfaceInGlobal ( spSink, IID_IDispatch, &dwGit );
    if ( FAILED(hr) ) return hr;

    m_csSinks.Lock();
    DWORD dwCookie = m_dwNextCookie++;
    m_sinks[dwCookie] = dwGit;
    m_csSinks.Unlock();

    *pdwCookie = dwCookie;
    return S_OK;
}

STDMETHODIMP CP2PHubCom::Unadvise ( DWORD dwCookie )
{
    DWORD dwGit = 0;

    m_csSinks.Lock();
    std::map<DWORD,DWORD>::iterator it = m_sinks.find ( dwCookie );
    bool bFound = ( it != m_sinks.end() );
    if ( bFound ) { dwGit = it->second; m_sinks.erase ( it ); }
    m_csSinks.Unlock();

    if ( !bFound ) return CONNECT_E_NOCONNECTION;

    IGlobalInterfaceTable *pGit = GetGit();
    if ( pGit != NULL ) pGit->RevokeInterfaceFromGlobal ( dwGit );
    return S_OK;
}

STDMETHODIMP CP2PHubCom::EnumConnections ( IEnumConnections **ppEnum )
{
    if ( ppEnum == NULL ) return E_POINTER;
    *ppEnum = NULL;
    return E_NOTIMPL;      // sinks are GIT cookies, not raw pointers to hand out
}
