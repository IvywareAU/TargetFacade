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
// ComHub.h -- CP2PHubCom, the coclass P2PHub.
//
// Wraps one p2pf::IP2PHub and turns its flat C++ sink into a COM
// connection-point event source.
//
// Two problems the flat facade leaves to its caller, solved here once:
//
//  1. THREADING.  p2pf callbacks arrive on the kernel pump thread, which is
//     not a COM apartment at all, so calling a client's sink from it would be
//     illegal for every STA client.  Every callback is therefore copied into a
//     queue and replayed on a dedicated MTA dispatch thread owned by this
//     object; sinks are held as Global Interface Table cookies, so the
//     re-fetch on the dispatch thread yields an apartment-correct proxy and
//     COM does the marshalling.  A client handler can block as long as it
//     likes without ever stalling the kernel pump.
//
//  2. PAYLOAD LIFETIME.  p2pf payload pointers are valid only for the duration
//     of the callback; the queued copy owns its bytes, so the SAFEARRAY handed
//     to the client is always valid.
//
// Only one connection point exists, so this object implements IConnectionPoint
// itself instead of carrying ATL's connection-point map.
#pragma once

#include "TargetCom_h.h"

// ---------------------------------------------------------------------------
// One queued callback, owning its own strings and bytes.
// ---------------------------------------------------------------------------
struct HubEvent
{
    enum Kind { evMessage, evPeerUp, evPeerDown, evError, evEvent, evTimer };

    Kind               kind;
    ATL::CComBSTR      a;            // source (message) / peer / what (error)
    ATL::CComBSTR      b;            // topic (message) / what (evEvent)
    std::vector<BYTE>  payload;
    VARIANT_BOOL       broadcast;
    LONG               n1;           // code (evEvent) / timerId (evTimer)
    LONG               n2;           // key  (evTimer)

    // --- facade ABI 7, evMessage only --------------------------------------
    // What IP2PHub::GetMsgInfo answered, captured ON THE PUMP THREAD INSIDE
    // THE DELIVERY -- the only place it may be asked -- and carried here so
    // the dispatch thread can still answer for it minutes later.  This copy
    // is the whole reason the MsgTag/MsgDest/... properties can exist at a
    // tier whose events are replayed rather than delivered.
    ATL::CComBSTR      msgDest;
    LONG               msgPriority;
    LONG               msgTag;
    LONG               msgFlags;

    // --- facade ABI 8, evMessage only --------------------------------------
    // The named fields, copied at the same moment and for the same reason.
    // Only populated when msgFlags said there were any, so a message without
    // fields costs the pump thread nothing at all.
    struct QueuedField
    {
        ATL::CComBSTR     name;
        std::vector<BYTE> value;
    };
    std::vector<QueuedField> fields;

    HubEvent ( ) : kind ( evError ), broadcast ( VARIANT_FALSE )
                 , n1 ( 0 ), n2 ( 0 )
                 , msgPriority ( 7 ), msgTag ( 0 ), msgFlags ( 0 ) { }
};

class CP2PNetworkCom;

class ATL_NO_VTABLE CP2PHubCom
    : public ATL::CComObjectRootEx<ATL::CComMultiThreadModel>
    , public ATL::IDispatchImpl<IP2PHubCom, &IID_IP2PHubCom, &LIBID_TargetComLib, 1, 0>
    , public ATL::ISupportErrorInfoImpl<&IID_IP2PHubCom>
    , public ATL::IProvideClassInfo2Impl<&CLSID_P2PHub, &DIID__IP2PHubEvents, &LIBID_TargetComLib>
    , public IConnectionPointContainer
    , public IConnectionPoint
{
  public:
    CP2PHubCom ( );

    BEGIN_COM_MAP(CP2PHubCom)
        COM_INTERFACE_ENTRY(IP2PHubCom)
        COM_INTERFACE_ENTRY(IDispatch)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
        COM_INTERFACE_ENTRY(IConnectionPointContainer)
        COM_INTERFACE_ENTRY(IConnectionPoint)
        COM_INTERFACE_ENTRY(IProvideClassInfo)
        COM_INTERFACE_ENTRY(IProvideClassInfo2)
    END_COM_MAP()

    DECLARE_PROTECT_FINAL_CONSTRUCT()

    void FinalRelease ( );

    // Called by CP2PNetworkCom right after construction.  Starts the dispatch
    // thread FIRST, then creates the kernel hub, so no event can be missed.
    HRESULT Init ( p2pf::IP2PNetwork *pNet, CP2PNetworkCom *pOwner, LPCWSTR wszAddress );

    // --- IP2PHubCom -------------------------------------------------------
    STDMETHOD(Listen)        ( BSTR toPeer, BSTR endpoint );
    STDMETHOD(Connect)       ( BSTR toPeer, BSTR endpoint );
    STDMETHOD(Send)          ( BSTR dest, BSTR topic, VARIANT payload );
    STDMETHOD(SendText)      ( BSTR dest, BSTR topic, BSTR text );
    STDMETHOD(Broadcast)     ( BSTR topic, VARIANT payload, VARIANT_BOOL *pDelivered );
    STDMETHOD(get_Address)   ( BSTR *pVal );
    STDMETHOD(IsPeerUp)      ( BSTR peer, VARIANT_BOOL *pVal );
    STDMETHOD(Close)         ( );

    // The read side.  Appended after Close, so dispids 1-8 and the vtable
    // prefix are undisturbed; each one is IP2PHub's caller-sized buffer
    // protocol turned into a single BSTR or LONG retval.
    STDMETHOD(RelationTo)      ( BSTR peer, LONG *pFlags );
    STDMETHOD(get_ConCount)    ( LONG *pVal );
    STDMETHOD(PeerAt)          ( LONG index, BSTR *pVal );
    STDMETHOD(EndpointFor)     ( BSTR peer, BSTR *pVal );
    STDMETHOD(get_Description) ( BSTR *pVal );

    // Past the messaging slice (facade ABI 6), appended again -- dispids
    // 14-20, IID unchanged.  Seven of the facade's ten: Post and GetNative
    // carry raw pointers, and OnMessageEx's answer cannot survive a queued
    // event tier (see the IDL).
    STDMETHOD(Disconnect)    ( BSTR peer );
    STDMETHOD(SetTimer)      ( LONG delayMillisecs, LONG key, LONG *pTimerId );
    STDMETHOD(KillTimer)     ( LONG timerId );
    STDMETHOD(Ping)          ( BSTR peer, LONG timeoutMillisecs, LONG *pMillisecs );
    STDMETHOD(SetConOption)  ( BSTR peer, LONG option, LONG value );
    STDMETHOD(GetConOption)  ( BSTR peer, LONG option, LONG *pValue );
    STDMETHOD(CloseIdleCons) ( );

    // The message model (facade ABI 7) -- dispids 21-26, IID unchanged.  The
    // four read properties answer about the message whose OnMessage event is
    // BEING RAISED, from the copy taken on the pump thread; outside one they
    // answer p2pfNoMessage.  See the IDL for why that is a different rule
    // from the flat ABI's and the same contract.
    STDMETHOD(get_MsgDest)     ( BSTR *pVal );
    STDMETHOD(get_MsgPriority) ( LONG *pVal );
    STDMETHOD(get_MsgTag)      ( LONG *pVal );
    STDMETHOD(get_MsgFlags)    ( LONG *pVal );
    STDMETHOD(SendEx)          ( BSTR dest, BSTR topic, VARIANT payload,
                                 LONG tag, LONG priority, LONG flags );
    STDMETHOD(BroadcastEx)     ( BSTR topic, VARIANT payload,
                                 LONG tag, LONG priority, LONG flags,
                                 VARIANT_BOOL *pDelivered );

    // Named fields (facade ABI 8) -- dispids 27-31, IID unchanged.  The three
    // read members answer from the copy captured at enqueue time, under the
    // same causality gate as the ABI 7 properties.
    STDMETHOD(SendMsg)           ( BSTR dest, BSTR topic, IP2PMessageCom *msg,
                                   LONG tag, LONG priority, LONG flags );
    STDMETHOD(BroadcastMsg)      ( BSTR topic, IP2PMessageCom *msg,
                                   LONG tag, LONG priority, LONG flags,
                                   VARIANT_BOOL *pDelivered );
    STDMETHOD(get_MsgFieldCount) ( LONG *pVal );
    STDMETHOD(MsgFieldName)      ( LONG index, BSTR *pVal );
    STDMETHOD(MsgField)          ( BSTR name, VARIANT *pVal );

    // --- IConnectionPointContainer ----------------------------------------
    STDMETHOD(EnumConnectionPoints) ( IEnumConnectionPoints **ppEnum );
    STDMETHOD(FindConnectionPoint)  ( REFIID riid, IConnectionPoint **ppCP );

    // --- IConnectionPoint --------------------------------------------------
    STDMETHOD(GetConnectionInterface)     ( IID *pIID );
    STDMETHOD(GetConnectionPointContainer)( IConnectionPointContainer **ppCPC );
    STDMETHOD(Advise)                     ( IUnknown *pUnkSink, DWORD *pdwCookie );
    STDMETHOD(Unadvise)                   ( DWORD dwCookie );
    STDMETHOD(EnumConnections)            ( IEnumConnections **ppEnum );

  private:
    // The flat facade sink.  Runs on the KERNEL PUMP THREAD: it may only copy
    // and enqueue -- never touch COM, never block.
    //
    // It implements the EXTENDED interface (facade ABI 6) and Init registers
    // it as both, which is what brings OnTimer and OnEvent to this tier.  Two
    // consequences of the facade's substitution rule are worth knowing:
    //   * OnMessage/OnError are no longer called once the extended sink is
    //     registered -- OnMessageEx/OnEvent are.  The two originals are kept
    //     (the interface requires them) and still forward correctly, so this
    //     object remains valid as a plain sink if the registration is ever
    //     dropped;
    //   * OnMessageEx always answers S_OK.  It CANNOT answer anything else:
    //     the client's sink does not run until the dispatch thread replays the
    //     event, long after the facade needs the answer.
    struct Sink : public p2pf::IP2PHubEvents2
    {
        virtual void OnMessage  ( const wchar_t *source, const wchar_t *topic
                                , const void *payload, unsigned int size
                                , bool broadcast );
        virtual void OnPeerUp   ( const wchar_t *peer );
        virtual void OnPeerDown ( const wchar_t *peer );
        virtual void OnError    ( const wchar_t *what );

        virtual HRESULT OnMessageEx ( const wchar_t *source, const wchar_t *topic
                                    , const void *payload, unsigned int size
                                    , bool broadcast );
        virtual void OnEvent    ( unsigned int code, const wchar_t *peer
                                , const wchar_t *what );
        virtual void OnTimer    ( unsigned int timerId, unsigned int key );
        virtual void OnPost     ( unsigned int key, void *context );

        CP2PHubCom *owner;
    };
    friend struct Sink;

    static unsigned __stdcall DispatchThunk ( void *pThis );
    void    DispatchLoop  ( );
    void    Push          ( const HubEvent& ev );
    bool    Pop           ( HubEvent& ev );
    void    Fire          ( const HubEvent& ev );
    void    CloseKernelHub( );          // idempotent; claims via m_lHubClosed

    // Guard for every IP2PHubCom entry point: fails cleanly after Close().
    HRESULT Live ( p2pf::IP2PHub **ppHub ) const;
    // The message event being raised on THIS thread, or p2pfNoMessage.
    HRESULT CurEvent ( const HubEvent **ppEv ) const;

    p2pf::IP2PHub  *m_pHub;
    CP2PNetworkCom *m_pOwner;           // weak; the network holds a ref to US
    Sink            m_sink;

    // dispatch thread
    HANDLE          m_hThread;
    HANDLE          m_hWake;
    unsigned        m_uThreadId;
    volatile LONG   m_lQuit;
    volatile LONG   m_lHubClosed;

    // queue (pump thread writes, dispatch thread reads)
    ATL::CComAutoCriticalSection m_csQueue;
    std::deque<HubEvent>         m_queue;
    LONG                         m_cDropped;

    // sinks, held as GIT cookies: our cookie -> GIT cookie
    ATL::CComAutoCriticalSection m_csSinks;
    std::map<DWORD, DWORD>       m_sinks;
    DWORD                        m_dwNextCookie;

    // --- facade ABI 7 -------------------------------------------------------
    // The message whose OnMessage event is being raised right now, for the
    // four Msg* properties.  Fire publishes it around the Invoke.
    //
    // THE GATE IS A CAUSALITY ID, NOT A THREAD ID, and the difference is the
    // whole reason this works.  The flat ABI can compare thread ids because
    // the client's handler runs ON the pump thread.  Here it does not run on
    // the dispatch thread either: the sink is a marshalled proxy, so Invoke
    // executes in the CLIENT's apartment, and when that handler reads a
    // property the call comes back as a fresh incoming call -- serviced by an
    // RPC pool thread that has never heard of the dispatch thread.  A thread
    // compare would reject every legitimate read and accept nothing.
    //
    // CoGetCurrentLogicalThreadId is what identifies the CALL CHAIN rather
    // than the thread running it: it is constant across every marshalling hop
    // of one causality, so the read that a message handler makes carries the
    // same id as the Invoke that reached it, however many apartments apart the
    // two threads are -- and a read from an unrelated thread carries its own.
    const HubEvent              *m_pCurEvent;
    GUID                         m_guidMsgCausality;
    volatile LONG                m_lMsgActive;   // 0 == no event being raised
};

// ---------------------------------------------------------------------------
// A NULL BSTR is a legal way for a client to say "empty string".
// ---------------------------------------------------------------------------
inline LPCWSTR Str ( BSTR bs ) { return ( bs != NULL ) ? bs : L""; }

// ---------------------------------------------------------------------------
// IErrorInfo, shared with ComNetwork.cpp.
//
// Both coclasses implement ISupportErrorInfo, so a failed call leaves a rich
// error object behind and a client reads a SENTENCE instead of a hex code:
// VB/VBScript get it in Err.Description (ITypeInfo::Invoke copies it into
// EXCEPINFO), .NET gets it as the COMException message, and a C++ vtable caller
// can call GetErrorInfo.  This matters far more here than in an ordinary COM
// layer: the facade's endpoint is a STRING, so its failures are runtime failures
// with nothing at the call site to inspect -- 0x80040208 alone tells a script
// author nothing about which part of "tpc://:7788" the parser objected to.
//
// ComFail names the call, echoes the arguments it was given, and spells out
// what the code means.  It is called on FAILED results only: P2PF_S_UNRELATED_
// LINK is a SUCCESS code and must pass through untouched.
// ---------------------------------------------------------------------------
HRESULT ComFail ( const IID& iid, HRESULT hr, LPCWSTR wszCall
                , BSTR bsArg1 = NULL, BSTR bsArg2 = NULL, BSTR bsArg3 = NULL );

// ---------------------------------------------------------------------------
// VARIANT <-> raw bytes, shared with ComNetwork.cpp.
//
// A payload VARIANT may be a SAFEARRAY of VT_UI1 (the canonical form, and what
// events deliver), a BSTR (sent as UTF-16 including its terminator, matching
// SendText), or empty.  BYREF and VARIANT-in-VARIANT wrappers are unwrapped,
// because that is what VB6/VBScript hand over.
// ---------------------------------------------------------------------------
HRESULT VariantToBytes ( const VARIANT& v, std::vector<BYTE>& out );
HRESULT BytesToVariant ( const void *pData, unsigned int cb, VARIANT *pOut );
