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
// FacadeInternal.h -- internal implementation classes of TargetFacade.dll.
//
// FacadeNetwork owns kernel startup/shutdown and the live hubs; FacadeHub
// (FacadeHub.h) is the P2PeerHub subclass behind IP2PHub.
//
// ---------------------------------------------------------------------------
// IMPLEMENTATION MAP (facade -> TargetCore)
// ---------------------------------------------------------------------------
//  P2PF_CreateNetwork          -> StartupP2Pmsg(16) + WSAStartup(2.2)
//  IP2PNetwork::Release (last) -> per-hub Close, CleanupP2Pmsg, WSACleanup
//
//  IP2PNetwork::CreateHub      -> address unique among live hubs?
//                                   (no -> P2PF_E_HUB_DUPLICATE; two live hubs
//                                    on one address corrupt the kernel's hub
//                                    registry and the kernel never checks)
//                                 new FacadeHub(address, events)  : P2PeerHub
//                                 hub->SpawnHub()   (fail -> P2PF_E_HUB_SPAWN)
//
//  BOTH arming verbs funnel through ONE 4-way switch, FacadeHub::MakeCon:
//
//    FacadeEndpoint{kind, host, name, num, derived} + bListen
//        tcp    listen -> P2PeerConWsa::ServiceFactory(peer, port)
//               dial   -> RetryDialWsa::Make(peer, host, port)
//        pipe   listen -> P2PeerConPipe::ServiceFactory(peer, pipeName)
//               dial   -> RetryDialPipe::Make(peer, pipeName)
//        dmx    listen -> P2PeerConDmx::ServiceFactory(peer, service)
//               dial   -> RetryDialDmx::Make(peer, service)       [derived]
//                      -> P2PeerConDmx::ClientFactory(peer, service)   [1-shot]
//        serial listen -> P2PeerCon232::ServiceFactory(peer, comPort)
//               dial   -> P2PeerCon232::ClientFactory(peer, comPort)   [1-shot]
//     ... `derived` is set by FacadeHub::ResolveEndpoint and by nothing else.
//         It is the whole difference between an endpoint that can be WRONG
//         (a caller typed it: fail fast) and one that can only be EARLY (the
//         facade resolved it against a live sibling hub: retry, on a budget).
//     ... then PostP2PeerCon(pCon):
//           factory NULL   -> P2PF_E_CON_FACTORY
//           post FALSE     -> P2PF_E_CON_DUPLICATE (wrap the never-posted con
//                             in SafeP2PeerCon so it is destroyed correctly --
//                             see TwoConTest.cpp PART B for why)
//     ... then record {endpoint, role} in FacadeHub::m_mapArmed, which is the
//         only place a hub can ever learn what it armed: the kernel's endpoint
//         fields are protected with no getters.
//
//  IP2PHub::Listen/Connect     -> FacadeEndpoint.cpp:ParseFacadeEndpoint, or
//                                 FacadeHub::ResolveEndpoint when the endpoint
//                                 is omitted; then the guards (empty toPeer,
//                                 toPeer holding ':' or '//', self-link, the
//                                 ancestor/descendant classification), then
//                                 the MakeCon/PostCon tail above
//           parse fail     -> P2PF_E_ENDPOINT
//           nothing to resolve to -> P2PF_E_UNRESOLVED
//           armed, but siblings   -> P2PF_S_UNRELATED_LINK (a SUCCESS code)
//                                    + one OnError
//
//  IP2PNetwork::Link           -> FindHubLocked x2 under m_oCSection
//                                 (a FacadeHub* is only valid while that lock
//                                  is held: Close() does RemoveHub + delete)
//                                 pre-flight: ConExists on both -> nothing is
//                                   armed unless both sides can be
//                                 ArmResolved(listener) then ArmResolved(dialer)
//                                 dialer failed -> FacadeHub::CloseCon unwinds
//                                   the listener; P2PF_E_LINK_PARTIAL only if
//                                   even that did not complete
//           address not live -> P2PF_E_NO_HUB
//
//  IP2PHub::Send/SendText      -> validate topic (no "P2Pmsg" prefix ->
//                                 P2PF_E_RESERVED_TOPIC), size <= MAX_PAYLOAD;
//                                 new P2PeerMsg32(thisAddr, dest, topic,
//                                                 payload, size);
//                                 PostP2PeerMsg(pMsg)
//                                 (topic IS the P2PmsgID -- PeerNode envelope)
//  IP2PHub::Broadcast          -> new P2PeerMsg32(thisAddr, dest=thisAddr,
//                                 P2Pmsg_BCast, ...) wrapping topic+payload,
//                                 PostP2PeerMsg -- match PeerNode's BCast
//                                 envelope so relays keep working
//
//  FacadeHub receive path (NO client-visible macros; the ONE map lives here):
//     BEGIN_P2PeerMsg_MAP entry ON_P2PeerMsg(L"*", On_AnyTopic) -- wildcard,
//       reserved "P2Pmsg*" names fall through to P2PeerHub defaults
//     On_AnyTopic     -> events->OnMessage(src, name, Data(), DataSize(), false)
//     On_P2PeerBCast  -> unwrap, events->OnMessage(..., true), then delegate
//                        to P2PeerHub base so tree relays keep running
//     On_P2PeerUCast  -> same, broadcast=false
//     On_P2PeerError  -> events->OnError(...)
//  Connection lifecycle (override P2PeerTarget virtuals, delegate to base
//  FIRST or LAST exactly as PeerNode does -- On_ConClose MUST delegate, the
//  auto-restart redial hangs off the base handler):
//     On_ConLogin     -> events->OnPeerUp(that)   [listening side]
//     On_ConLoginAck  -> events->OnPeerUp(that)   [dialing side]
//     On_ConClose     -> events->OnPeerDown(...)
//
//  --- ABI 6 (missing.md items 1-6) ------------------------------------------
//  IP2PHub::Disconnect         -> ConExists, then FacadeHub::CloseCon
//                                 (P2PsigCon_DESTROY + a bounded wait), then
//                                 MarkPeer(false) if the peer had been up.
//                                 Refuses the pump thread: it WAITS on the
//                                 pump -> P2PF_E_PUMP_THREAD
//  IP2PHub::SetExtEvents       -> m_pExtEvents, under m_oCSectPeers.  Once set,
//                                 OnMessage/OnError are delivered as
//                                 OnMessageEx/OnEvent INSTEAD (never both)
//  IP2PHub::SetTimer/KillTimer -> post "P2PF$Timer"/"P2PF$Kill" to THIS hub;
//  IP2PHub::Post                  post "P2PF$Post";
//  IP2PHub::Ping                  post "P2PF$Ping", far side answers
//                                 "P2PF$Pong" -- HandlePrivate is the whole
//                                 receive half.  All three are client-thread
//                                 calls whose work must happen on the pump,
//                                 and a P2PeerMsg is the kernel's one ordered
//                                 way in (SetPITimer resolves pump 0 = "the
//                                 pump on THIS thread", so it cannot be armed
//                                 from anywhere else at all)
//     ... "P2PF$" is the facade's own reserved namespace: Send/Broadcast
//         refuse it, and the three self-topics are refused unless the message
//         source is this hub's own address
//  IP2PHub::Set/GetConOption   -> ConQuery(peer) -> P2PeerCon::GetState/GetMode
//                                 or P2Peerio::SetIFTrace/SetMax*Size/
//                                 IsEncrypted
//  IP2PHub::CloseIdleCons      -> P2PeerHub::PauseHub (which is
//                                 ConSignal("*", P2PsigCon_CLOSEONIDLE), not a
//                                 pause).  No counterpart: WakeupHub is an
//                                 ASSERT(0) stub, P2PeerHub.cpp:248-262
//  IP2PHub::GetNative          -> the P2PeerHub subobject, unguarded and
//                                 undocumented past the cast, on purpose
//
//  --- ABI 7 (missing.md item 7a: the message model) -------------------------
//  IP2PHub::SendEx             -> Send/Broadcast's own guards, then PostMsg
//  IP2PHub::BroadcastEx           with a MsgOpts:
//                                   priority -> P2PeerMsg::SetPriority
//                                   tag      -> P2PeerMsg::SetAddrTag (the
//                                               DWORD_PTR in P2PeerMsgPrefix;
//                                               it DOES cross a TCP wire --
//                                               measured, FacadeSmokeTest 23)
//                                   NO_BOUNCE-> P2PeerMsg_SetCtrlOptions, top
//                                               bit of uiCtrl.  NOT the
//                                               kernel's own toggle:
//                                               P2PeerMsg::Exceptions() is
//                                               declared and implemented
//                                               NOWHERE (compiles, then fails
//                                               to link), and nothing anywhere
//                                               consults P2PeerMsgCtrl_
//                                               EXCEPTIONS
//     ... each property is applied only if asked for, so a SendEx with the
//         defaults posts byte-for-byte what Send posts
//     ... NO_BOUNCE is honoured on RECEIPT, at the two places the facade
//         generates a bounce: On_AnyTopic (stops a declined unicast reaching
//         the kernel's NotHandled) and ReportDeclined (the broadcast path).
//         It cannot cover an unroutable address -- RouteP2PeerMsg posts that
//         one before any facade code runs
//  IP2PHub::GetMsgInfo         -> the P2PeerMsg being delivered: GetDestin,
//                                 Priority, AddrTag, IsWrapped/IsReflected.
//                                 Published by CurMsgScope around the client
//                                 callback in Deliver, gated on the thread id
//                                 -- outside a delivery, P2PF_E_NO_MESSAGE
//     ... a METHOD on IP2PHub rather than a wider callback, because IP2PHub is
//         implemented HERE and IP2PHubEvents is implemented by the client.
//         There is deliberately no IP2PHubEvents3
//
//  --- ABI 8 (missing.md item 7b: named fields) ------------------------------
//  IP2PNetwork::CreateMessage  -> new FacadeMessage.  A VALUE: no hub, no
//                                 kernel object, no lock, so the network keeps
//                                 no list of them and closes none
//  IP2PHub::SendMsg            -> Send's guards, CheckTotalSize (payload +
//  IP2PHub::BroadcastMsg          every field value must fit ONE P2PeerMsg),
//                                 then PostMsg with MsgOpts::pFields
//     ... the fields become a child of the message ROOT named "P2PF$Fields",
//         one DeclareItem per field, plus a "P2PF$Names" sibling holding the
//         names tab-separated.  NOT in VBLockBSTR_MSG: that is Data()/
//         DataSize(), i.e. what every existing peer reads, and it must stay
//         byte-identical so fields can never break a receiver
//     ... the name index exists because P3PmsgField HAS NO CHILD ENUMERATION.
//         Read the right header: "P2Pmsg.h" resolves to ..\Msgcore\P2Pmsg.h,
//         where class P3PmsgNode is entirely COMMENTED OUT and P3PmsgItem is a
//         typedef for P3PmsgField -- no GetCount, no cursor (P3PmsgCurs is
//         forward-declared there and never defined)
//     ... P3PmsgData's constructors overload on LPCSTR/LPCWSTR/const void*,
//         and the string ones take a length in CHARACTERS.  Every blob is
//         cast to (const void*) or it is silently stored at twice its size
//  IP2PHub::GetFieldCount      -> the same current-message gate as GetMsgInfo
//  IP2PHub::GetFieldName          (CurMsg), then the index / SelectItem.
//  IP2PHub::GetField              P2PF_E_NO_FIELD and "present but empty" are
//                                 DIFFERENT answers, on purpose
//
//  IP2PHub::Close              -> DrainCons() (retire live connections while
//                                   the pump can still process the request --
//                                   the kernel's own teardown sweep walks a
//                                   list its Drop() deletes out of);
//                                 CloseHub(); WaitForSingleObject(pumpThread);
//                                 delete
//
//  Threading note: every exported entry point (factory + vtable methods) must
//  open with AFX_MANAGE_STATE(AfxGetStaticModuleState()) -- regular MFC DLL.
// ---------------------------------------------------------------------------
#pragma once

#include "TargetFacade.h"
#include "FacadeEndpoint.h"

class FacadeHub;

// ---------------------------------------------------------------------------
// FacadeNetwork -- process-wide refcounted singleton behind P2PF_CreateNetwork
// ---------------------------------------------------------------------------
//
//  Hand one string back through the caller's buffer
//  NOTES: The plain Win32 in/out protocol: *cch in is capacity, *cch out is
//         what was needed -- ALWAYS, so one failed call tells you exactly how
//         big to make the second one
//       : Nothing is written on a short buffer.  A partial string is worse
//         than none: it looks like a value
//       : Shared rather than per-file since the network grew a read side of
//         its own; every string-out method in the ABI is this one function
//
inline HRESULT
CopyOut ( const wchar_t *lpszValue, wchar_t *buf, unsigned int *cch )
{
    if ( !cch )
      return E_POINTER;

    unsigned int uNeed = (unsigned int)::wcslen ( lpszValue ) + 1;
    unsigned int uHave = buf ? *cch : 0;
    *cch = uNeed;                       // reported whatever happens next

    if ( !buf )
      return S_OK;                      // size query
    if ( uHave < uNeed )
      return HRESULT_FROM_WIN32 ( ERROR_MORE_DATA );

    ::wcscpy_s ( buf, uHave, lpszValue );
    return S_OK;
}

//
//  Hand one BLOB back through the caller's buffer
//  NOTES: CopyOut's twin for the one thing in this ABI that is bytes rather
//         than characters -- a field value (ABI 8).  Same protocol exactly:
//         *size in is capacity, *size out is what was needed ALWAYS, a NULL
//         buffer is a size query, and a short buffer writes nothing
//       : A zero-length value is a real value and answers S_OK with *size 0.
//         That is the difference between a field that is present and empty and
//         one that is absent, which answers P2PF_E_NO_FIELD -- and the reason
//         this cannot simply reuse the string version
//
inline HRESULT
CopyOutBytes ( const void *pvValue, unsigned int uSize
             , void *buf, unsigned int *size )
{
    if ( !size )
      return E_POINTER;

    unsigned int uHave = buf ? *size : 0;
    *size = uSize;                      // reported whatever happens next

    if ( !buf )
      return S_OK;                      // size query
    if ( uHave < uSize )
      return HRESULT_FROM_WIN32 ( ERROR_MORE_DATA );

    if ( uSize )
      ::memcpy ( buf, pvValue, uSize );
    return S_OK;
}

class FacadeNetwork : public p2pf::IP2PNetwork
{
    public:
        // Acquire (create on first call) / release the singleton.
        static HRESULT
          Acquire ( p2pf::IP2PNetwork **outNetwork );

    // IP2PNetwork
    public:
        virtual HRESULT
          CreateHub ( const wchar_t *address
                    , p2pf::IP2PHubEvents *events
                    , p2pf::IP2PHub **outHub );
        virtual ULONG
          Release   ( );
        virtual const wchar_t*
          VersionString ( ) const;
        virtual HRESULT
          Link      ( const wchar_t *listenerAddr
                    , const wchar_t *dialerAddr
                    , const wchar_t *endpoint );
        virtual HRESULT
          SetEndpoint    ( const wchar_t *address, const wchar_t *endpoint );
        virtual HRESULT
          SetEndpointMap ( const wchar_t *text, unsigned int *badLine );
        virtual HRESULT
          GetEndpointFor ( const wchar_t *address
                         , wchar_t *buf, unsigned int *cch ) const;
        virtual HRESULT
          CreateMessage  ( p2pf::IP2PMessage **outMessage );
        virtual HRESULT
          CreateHubEx    ( const wchar_t *address
                         , p2pf::IP2PHubEvents *events
                         , unsigned int flags
                         , p2pf::IP2PHub **outHub );

        // --- ABI 10 ---------------------------------------------------------
        virtual HRESULT
          SetDiagSink    ( p2pf::IP2PDiagEvents *sink, unsigned int mask );
        virtual HRESULT
          SetDiagMask    ( unsigned int mask );
        virtual HRESULT
          GetDiagMask    ( unsigned int *outMask ) const;
        virtual HRESULT
          GetDiagText    ( unsigned int part
                         , wchar_t *buf, unsigned int *cch ) const;
        virtual HRESULT
          GetDiagInfo    ( unsigned int *outHResult
                         , unsigned int *outTime
                         , unsigned int *outThreadId ) const;
        virtual HRESULT
          RaiseDiag      ( unsigned int severity
                         , const wchar_t *module, const wchar_t *text );
        virtual HRESULT
          IsDiagWanted   ( unsigned int mask, unsigned int *outMatched ) const;

        // --- ABI 11 ---------------------------------------------------------
        virtual HRESULT
          SetSecurityDir  ( const wchar_t *dir );

    // Hub bookkeeping
    public:
        // Drop a hub that is closing itself (called from FacadeHub::Close).
        void
          RemoveHub ( FacadeHub *pHub );
        // Resolve an endpoint-less Connect: what should `dialerAddr` dial
        // to reach the in-process hub `peerAddr`?  P2PF_E_UNRESOLVED when
        // there is no such hub, or it has armed no listener expecting us.
        HRESULT
          ResolveDial ( const wchar_t *peerAddr, const wchar_t *dialerAddr
                      , FacadeEndpoint& rEp );
        // The CONFIGURED dial endpoint for `address`, parsed.
        // P2PF_E_UNRESOLVED when the deployment map has no entry for it --
        // which is every address until someone calls SetEndpoint*.
        HRESULT
          LookupEndpoint ( const wchar_t *address, FacadeEndpoint& rEp ) const;

        // --- ABI 11 : security ----------------------------------------------
        // The two paths a SECURE hub needs -- the directory its key material
        // lives in, and the revocation list shared by every secure hub of the
        // process, which this guarantees exists.  Takes m_oCSection itself, so
        // it is for callers that do NOT hold it (CreateHubEx, and the arming
        // verbs on a hub).  P2PF_E_SECURITY with the reason on the diagnostic
        // stream when either cannot be made.
        HRESULT
          SecurityPaths ( CString& rcsDir, CString& rcsRevoke );
        // One sentence about a security refusal, onto the diagnostic stream as
        // an error.  P2PF_E_SECURITY is one code covering a family of file
        // problems, and a code with no sentence sends an operator looking
        // through a directory by hand -- which is the failure the kernel's own
        // arming refusal already names the file to prevent.  Public because
        // FacadeHub raises them too: the provisioning is the hub's.
        void
          RaiseSecurityError ( const wchar_t *what );

    private:
        FacadeNetwork ( ) { }
       ~FacadeNetwork ( ) { }

        BOOL
          Startup  ( );   // StartupP2Pmsg + WSAStartup
        void
          Shutdown ( );   // close hubs, CleanupP2Pmsg, WSACleanup
        // The one callback the kernel's single P2Pevent slot calls, on the
        // thread that raised the event.  Static because the slot is static:
        // there is one of these per process, not one per network.   (ABI 10)
        static void WINAPI
          DiagCallback ( P2PeventSinkID nSinkID, DWORD_PTR dwKey
                       , const P2Pevent& rEvent );
        // Filter, publish as "the record being delivered on this thread", and
        // hand to the client's sink.  Called only by DiagCallback.
        void
          DeliverDiag ( const P2Pevent& rEvent );
        void
          CloseAllHubs ( );

        // --- ABI 11 : security ----------------------------------------------
        // Resolve (and create) the directory the key material lives in, once.
        // The caller MUST hold m_oCSection.  P2PF_E_SECURITY when the
        // directory cannot be resolved or made.
        HRESULT
          EnsureSecurityDirLocked ( CString& rcsDir );
        // Hand each hub of one edge the other's public points, and turn
        // enforcement on for both.  NOTHING IS ARMED BY THIS -- it either
        // returns S_OK, at which point the caller may arm, or it returns
        // P2PF_E_SECURITY having changed neither hub.  Called only for a pair
        // that is already known to be BOTH secure.  The caller MUST hold
        // m_oCSection.
        HRESULT
          SecureEdgeLocked ( FacadeHub *pA, FacadeHub *pB );
        // Make sure the shared revocation list EXISTS, so naming it is a
        // POSITION rather than a self-inflicted outage: a configured list that
        // will not load fails closed and refuses every peer, and an
        // all-comments file is the honest "nothing revoked yet".
        static BOOL
          EnsureRevocationList ( const CString& csPath );
        // The live hub answering to `address`, or NULL.
        // NOTES: The caller MUST hold m_oCSection, and must not let go of it
        //        while it still holds the returned pointer -- FacadeHub::Close
        //        runs RemoveHub and then `delete this`
        FacadeHub*
          FindHubLocked ( const wchar_t *address );
        // TRUE when a live hub already answers to `address`.
        // NOTES: The caller MUST hold m_oCSection
        BOOL
          IsAddressTakenLocked ( const wchar_t *address );

    // Attributes
    private:
        LONG                    m_cRef    = 0;
        BOOL                    m_bWsaUp  = FALSE;
        BOOL                    m_bMsgUp  = FALSE;
        CList<FacadeHub*>       m_listHubs;          // hubs still open
        FacadeEndpointMap       m_mapEndpoints;      // the deployment map
        mutable CCriticalSection m_oCSection;        // guards both of those

        // --- ABI 10 ---------------------------------------------------------
        // The diagnostics sink and its filter.  Read on EVERY raised event, on
        // whatever thread raised it, so they are read without taking
        // m_oCSection: an event can be raised from inside a call that already
        // holds it (every failure path in this file is a candidate), and a
        // logger that deadlocks the thing it is logging is worse than no
        // logger.  Both are pointer-sized and are published with interlocked
        // stores; a sink that changes while an event is in flight delivers to
        // the old one or the new one, never to a torn pointer.
        //
        // Which is also the whole of why SetDiagSink(NULL) is not optional
        // housekeeping: nothing here can tell a freed sink from a live one.
        p2pf::IP2PDiagEvents * volatile m_pDiagSink  = 0;
        volatile LONG                   m_lDiagMask  = 0;
        // Whether THIS network holds the kernel's one static callback slot,
        // so Shutdown can give it back exactly once.
        volatile LONG                   m_lDiagHeld  = 0;
        // Deliveries currently inside the client's sink, on any thread.  What
        // makes SetDiagSink(NULL) safe to follow with a delete of that sink:
        // an unregister that observes zero knows nobody is in there.
        volatile LONG                   m_lDiagBusy  = 0;
        // The serial handed to OnDiag.  It is the FACADE's, counted here, and
        // that is not a preference: P2Pevent::GetEvent is documented as
        // "internally allocated event number ... auto-assigned in MakeEvent"
        // and the field behind it (P2PeventNode::nEventNo) is ASSIGNED
        // NOWHERE -- Msgexception.cpp declares a counter for it at :32 and
        // never increments it, so GetEvent answers 0 for every event ever
        // raised.  Counted for EVERY event that reaches the slot, before the
        // mask is applied, so a jump in the numbers a client sees is exactly
        // the count of what its own filter dropped.
        volatile LONG                   m_lDiagSeq   = 0;

        // --- ABI 11 ---------------------------------------------------------
        // Where secure links keep their key material, with a trailing
        // separator.  Empty until the first secure LinkEx resolves it, or
        // until SetSecurityDir states it.  Guarded by m_oCSection, like
        // everything else a Link touches.
        CString                 m_csSecDir;
        // A hub has been provisioned out of m_csSecDir, so the directory can
        // no longer move: those hubs hold keys loaded from it, and a call that
        // appeared to relocate them but did not would be worse than a refusal.
        BOOL                    m_bSecUsed = FALSE;

        static FacadeNetwork   *s_pInstance;         // the singleton
        static CCriticalSection s_oCSectInstance;    // guards s_pInstance
};
