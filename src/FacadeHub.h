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
// FacadeHub.h -- the private P2PeerHub subclass behind p2pf::IP2PHub.
//
// This is the ONE place in the product where TargetCore's message-map macros
// live: a single wildcard entry (`"*"` -> On_AnyTopic). Clients get plain
// virtuals / std::function handlers instead, which is the whole point of the
// facade.
//
// Two base classes on purpose:
//   * P2PeerHub    -- what the kernel drives (pump thread, maps, cons)
//   * p2pf::IP2PHub-- what the client holds (flat vtable, HRESULTs)
// The client only ever sees the IP2PHub subobject; nothing of P2PeerHub's
// layout is reachable from the public header.
#pragma once

#include "TargetFacade.h"
#include "FacadeEndpoint.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class FacadeNetwork;

class FacadeHub : public P2PeerHub, public p2pf::IP2PHub
{
    // Constructors and destructor
    public:
        FacadeHub ( P2PaddrSTR strAddress
                  , p2pf::IP2PHubEvents *pEvents
                  , FacadeNetwork *pOwner );
      virtual
       ~FacadeHub ( );

    // Lifecycle (facade side)
    public:
        // Start the pump thread. FALSE -> the hub is unusable.
        BOOL
          Start ( );
        // Start WITHOUT a thread: create the hub in the CALLING thread's
        // context and leave it inert until that thread calls Pump. FALSE ->
        // the hub is unusable (most often because this thread already owns a
        // pump; the kernel allows exactly one per thread).      (ABI 9)
        BOOL
          StartCallerPumped ( );
        // Tear down without touching the owner's hub list (used when the
        // network itself is shutting down and already holds that list).
        void
          CloseInternal ( );

    // p2pf::IP2PHub
    public:
      virtual HRESULT Listen        ( const wchar_t *toPeer, const wchar_t *endpoint );
      virtual HRESULT Connect       ( const wchar_t *toPeer, const wchar_t *endpoint );

      virtual HRESULT Send          ( const wchar_t *dest, const wchar_t *topic
                                    , const void *payload, unsigned int size );
      virtual HRESULT SendText      ( const wchar_t *dest, const wchar_t *topic
                                    , const wchar_t *text );
      virtual HRESULT Broadcast     ( const wchar_t *topic
                                    , const void *payload, unsigned int size );

      virtual const wchar_t*
                      Address       ( ) const;
      virtual BOOL    IsPeerUp      ( const wchar_t *peer ) const;
      virtual HRESULT Close         ( );
      virtual HRESULT GetConCount   ( unsigned int *outCount ) const;
      virtual HRESULT GetCon        ( unsigned int index
                                    , wchar_t *peerBuf,     unsigned int *peerCch
                                    , wchar_t *endpointBuf, unsigned int *endpointCch
                                    , unsigned int *outFlags ) const;
      virtual HRESULT GetEndpoint   ( const wchar_t *peer
                                    , wchar_t *buf, unsigned int *cch ) const;
      virtual HRESULT Describe      ( wchar_t *buf, unsigned int *cch ) const;

      // --- ABI 6 ----------------------------------------------------------
      virtual HRESULT Disconnect    ( const wchar_t *peer );
      virtual HRESULT SetExtEvents  ( p2pf::IP2PHubEvents2 *events2 );
      virtual HRESULT SetTimer      ( unsigned int delayMillisecs
                                    , unsigned int key
                                    , unsigned int *outTimerId );
      virtual HRESULT KillTimer     ( unsigned int timerId );
      virtual HRESULT Post          ( unsigned int key, void *context );
      virtual HRESULT SetConOption  ( const wchar_t *peer
                                    , unsigned int option, unsigned int value );
      virtual HRESULT GetConOption  ( const wchar_t *peer
                                    , unsigned int option
                                    , unsigned int *outValue ) const;
      virtual HRESULT Ping          ( const wchar_t *peer
                                    , unsigned int timeoutMillisecs
                                    , unsigned int *outMillisecs );
      virtual HRESULT CloseIdleCons ( );
      virtual HRESULT GetNative     ( void **outNativeHub ) const;

      // --- ABI 7 ----------------------------------------------------------
      virtual HRESULT SendEx        ( const wchar_t *dest, const wchar_t *topic
                                    , const void *payload, unsigned int size
                                    , unsigned int priority, unsigned int tag
                                    , unsigned int flags );
      virtual HRESULT BroadcastEx   ( const wchar_t *topic
                                    , const void *payload, unsigned int size
                                    , unsigned int priority, unsigned int tag
                                    , unsigned int flags );
      virtual HRESULT GetMsgInfo    ( wchar_t *destBuf, unsigned int *destCch
                                    , unsigned int *outPriority
                                    , unsigned int *outTag
                                    , unsigned int *outFlags ) const;

      // --- ABI 8 ----------------------------------------------------------
      virtual HRESULT SendMsg       ( const wchar_t *dest, const wchar_t *topic
                                    , p2pf::IP2PMessage *msg
                                    , unsigned int priority, unsigned int tag
                                    , unsigned int flags );
      virtual HRESULT BroadcastMsg  ( const wchar_t *topic
                                    , p2pf::IP2PMessage *msg
                                    , unsigned int priority, unsigned int tag
                                    , unsigned int flags );
      virtual HRESULT GetFieldCount ( unsigned int *outCount ) const;
      virtual HRESULT GetFieldName  ( unsigned int index
                                    , wchar_t *buf, unsigned int *cch ) const;
      virtual HRESULT GetField      ( const wchar_t *name
                                    , void *buf, unsigned int *size ) const;

      // --- ABI 9 ----------------------------------------------------------
      virtual HRESULT Pump          ( unsigned int timeoutMillisecs
                                    , unsigned int *outWhat );
      virtual HRESULT GetPending    ( unsigned int *outCount ) const;
      virtual HRESULT GetPumpInfo   ( unsigned int *outFlags
                                    , unsigned int *outThreadId ) const;

    // Facade-internal arming (used by FacadeNetwork::Link)
    public:
        // Arm one connection from an ALREADY parsed/resolved endpoint, doing
        // the guards and the topology classification but no resolution.
        // S_OK, P2PF_S_UNRELATED_LINK, or a failure.
        HRESULT
          ArmResolved ( const wchar_t *peer, const FacadeEndpoint& rEp
                      , bool bListen );
        // How long CloseCon waits for a signalled connection to leave the
        // hub, in the same 25 ms slice the drain budget uses (FacadeHub.cpp).
        // TWO of them, because CloseCon has two callers retracting two
        // different things and one budget served the wrong one:
        //
        //   kUnwindSlices  LINK'S UNWIND retracts a connection that has never
        //                  connected. No session, only a registration to
        //                  drop. This is the case the single budget was sized
        //                  for, and it was right about it.
        //   kRetireSlices  DISCONNECT (ABI 6) retires a LIVE connection --
        //                  which on a listening hub is TWO of them, the armed
        //                  service AND the accepted clone AcceptSpawn posted
        //                  beside it (see FindCon), both answering to one peer
        //                  address, and neither off the hub's list until its
        //                  last reference goes. Measured on the named dmx
        //                  listener the smoke test's ABI 6 section builds:
        //                  still there at 1.5 s, gone by 2 s. The short budget
        //                  therefore expired on a disconnect that had in fact
        //                  worked. It gets the drain budget instead, for the
        //                  reason the drain has one -- overshooting costs a
        //                  slow call, undershooting costs correctness.
        enum { kUnwindSlices =  40      // 1 s -- a registration
             , kRetireSlices = 120 };   // 3 s -- a session

        // Retract one connection. Best effort, bounded: FALSE means the
        // connection is still on the hub and the caller must say so rather
        // than pretend the arming never happened.
        BOOL
          CloseCon ( const wchar_t *peer, int nSlices );
        // The canonical endpoint string this hub armed as a LISTENER for
        // `peer`, or empty. Empty is also the honest answer for a peer this
        // hub dialed, and for one that arrived over a connection this hub
        // never armed (an accepted clone, or a peer reached by routing).
        CString
          ArmedListener ( const wchar_t *peer ) const;

    // Internals
    private:
        // Arm one connection: enable the BCast/UCast relays on it, hand it to
        // the kernel, and translate the outcome into an HRESULT.
        HRESULT
          PostCon ( P2PeerCon *pCon );
        // THE 4-way switch -- the entire difference one transport makes, in
        // one place. NULL on an unknown transport.
        P2PeerCon*
          MakeCon ( const FacadeEndpoint& rEp, const wchar_t *peer
                  , bool bListen );
        // Record + MakeCon + PostCon, rolling the record back if the post is
        // refused. No guards, no classification -- ArmResolved does those.
        HRESULT
          ArmRecorded ( const wchar_t *peer, const FacadeEndpoint& rEp
                      , bool bListen );
        // Listen/Connect: guards, resolution, ArmResolved.
        HRESULT
          ArmUnified ( const wchar_t *peer, const wchar_t *endpoint
                     , bool bListen );
        // Fill rEp for an omitted endpoint (the C3 in-process tier).
        HRESULT
          ResolveEndpoint ( const wchar_t *peer, bool bListen
                          , FacadeEndpoint& rEp );

    // Read side
    private:
        // One row of what this hub knows.
        struct ConRow
        {
            std::wstring strPeer;
            std::wstring strEndpoint;   // empty: this hub did not arm it
            unsigned int uFlags;        // P2PF_CON_* | exactly one P2PF_REL_*
        };
        // Every peer this hub armed a connection for, plus every one it
        // learned at login, in one ordered pass under one lock. Ordering is
        // the maps' own (ordinal by peer address), so it is stable for as
        // long as the set is.
        void
          SnapshotCons ( std::vector<ConRow>& rOut ) const;
        // The three per-message properties ABI 7 added, in the one shape they
        // travel in. A NULL MsgOpts* means "post the message the kernel's
        // constructor built", which is what keeps a plain Send byte-identical
        // to what ABI 6 put on the wire -- and what SendEx with all three
        // defaults reduces to.
        struct MsgOpts
        {
            unsigned int uPriority;   // P2PF_PRI_DEFAULT == do not touch it
            unsigned int uTag;        // 0 == do not touch it
            unsigned int uFlags;      // P2PF_SEND_*
            // The named fields to hang off the message, or NULL (ABI 8).
            // A pointer to the caller's object rather than a copy: the
            // message is built and posted inside one call, and the caller
            // holds its IP2PMessage across the whole of it.
            const class FacadeMessage *pFields;
        };
        // Build + post one topic-named message. Shared by Send/SendText and
        // (with bBCast) by Broadcast.
        HRESULT
          PostMsg ( const wchar_t *dest, P2PmsgID strName
                  , const void *pvData, unsigned int uSize
                  , const MsgOpts *pOpts = 0 );
        // Frame topic+payload and post one copy per logged-in peer. The whole
        // of Broadcast and BroadcastEx.
        HRESULT
          BroadcastInternal ( const wchar_t *topic
                            , const void *payload, unsigned int size
                            , const MsgOpts *pOpts );
        // Publish one message as "the one being delivered" for the length of
        // one client callback, and take it down again on every exit path --
        // including the one where the client's handler throws, which is the
        // reason this is a scope object and not two statements.
        class CurMsgScope
        {
          public:
            CurMsgScope ( FacadeHub& rHub, P2PeerMsg *pMsg, bool bBCast );
           ~CurMsgScope ( );
          private:
            FacadeHub  &m_rHub;
            P2PeerMsg  *m_pPrevMsg;
            bool        m_bPrevBCast;
            LONG        m_lPrevThread;
        };
        friend class CurMsgScope;
        // Surface one decoded message to the client sink.  S_OK == handled
        // (the ABI 5 behaviour, and what a plain sink always means); S_FALSE
        // == an extended sink declined it and routing should continue.
        HRESULT
          Deliver ( P2PeerMsg *pMsg, const wchar_t *lpszTopic
                  , const void *pvPayload, unsigned int uSize, bool bBCast );
        // Report one condition: OnEvent(code, peer, what) to an extended sink,
        // or the prose half alone to a plain one.  The substitution rule lives
        // here and in Deliver, and nowhere else.
        void
          RaiseEvent ( unsigned int uCode, const wchar_t *lpszPeer
                     , const wchar_t *lpszWhat );
        // The extended sink, copied out under the lock.  NULL when none is
        // registered, which is the ABI 5 shape.
        p2pf::IP2PHubEvents2*
          ExtEvents ( ) const;
        // ONE private-topic message, already known to be ours, dispatched on
        // the pump thread.  TRUE when it was consumed here.
        BOOL
          HandlePrivate ( P2PeerMsg *pMsg, const wchar_t *lpszTopic );
        // Arm the KERNEL timer for one facade timer row, compensating for the
        // one-second granularity of the deadline the kernel computes.  Pump
        // thread only (SetPITimer resolves "pump 0" as the calling thread's).
        // 0 == not armed.
        PITimerID
          ArmPumpTimer ( ULONGLONG uDeadlineTick, unsigned int uTimerId
                       , bool bCompensate );
        // Report one message the client sink DECLINED, on a path where the
        // decline cannot simply be passed down the handler chain.  The kernel's
        // own not-handled report, sent from where the kernel would have sent
        // it.  Pump thread only.
        void
          ReportDeclined ( P2PeerMsg *pMsg );
        // Surface one P2Pmsg_Exception -- a message this hub sent, bounced
        // back by the kernel -- as P2PF_EVT_ROUTING_ERROR.  Pump thread only.
        void
          ReportBounce ( P2PeerMsg *pMsg );
        // WHICH connection a per-peer question is about.
        //
        // A hub can hold MORE THAN ONE connection answering to one peer
        // address, and the kernel's own ConQuery hands back whichever is
        // first -- see FindCon.
        enum ConPick
        {
            pickSession,   // the one carrying traffic (ConState_Login)
            pickArmed      // the one THIS hub armed (not accept-spawned)
        };
        // The connection for `peer` that answers the question being asked.
        // Returns FALSE when the hub has none at all.
        BOOL
          FindCon ( const wchar_t *peer, ConPick ePick
                  , SafeP2PeerCon& rSafe ) const;
        // The message being delivered on THIS thread, or NULL -- the one gate
        // behind GetMsgInfo and the three field readers (ABI 8).
        P2PeerMsg*
          CurMsg ( ) const;
        // Payload + every field value must fit ONE P2PeerMsg. A per-field cap
        // alone would let 64 legal fields build one illegal message, and the
        // kernel's answer to an oversize frame is to drop the connection.
        static HRESULT
          CheckTotalSize ( const class FacadeMessage *pMsg );
        // The facade's field item on one message, or NULL when it carries
        // none. `bCreate` is for the SEND path, where it is being built.
        // P3PmsgItem, not P3PmsgNode: the node class is commented out in the
        // P2Pmsg.h that actually compiles -- see kFieldsItem.
        static P3PmsgItem*
          FieldsItem ( P2PeerMsg *pMsg, bool bCreate );
        // The tab-separated field-name index the facade writes beside them,
        // because the real P3PmsgField offers no child enumeration at all.
        static BOOL
          FieldNames ( P2PeerMsg *pMsg, CStringArray& rOut );
        // Read/write one P2Peerio knob on the connection for `peer`.
        HRESULT
          ConOption ( const wchar_t *peer, unsigned int option
                    , unsigned int *pValue, bool bSet ) const;
        // Flip the recorded up/down state of a peer and notify the client.
        void
          MarkPeer ( P2PaddrSTR strPeer, bool bUp );
        // Retire every connection on this hub while its pump can still
        // process the request. FALSE == the drain budget ran out.
        BOOL
          DrainCons ( );

        // --- ABI 9 ----------------------------------------------------------
        // TRUE when the calling thread is the one this hub's pump runs on --
        // the thread the facade spawned, or, for a caller-pumped hub, the one
        // that created it. GetHubID() answers for both, because the kernel
        // stamps a hub's id with the id of the thread it was created on
        // either way (CreateP2PmsgHub via P2PmsgHubMgr's constructor).
        bool
          OwnsPump ( ) const;
        // Spend `uMillisecs` waiting -- and on a caller-pumped hub owned by
        // this thread, spend it PUMPING.
        //
        // This is the whole of what makes Disconnect, Ping and Close work on a
        // caller-pumped hub. Every wait in this file exists because some other
        // thread has to make progress before this one can continue; when the
        // waiter IS the pump, sleeping through the slice guarantees that
        // progress never happens, so the wait must do the work itself.
        void
          WaitSlice ( unsigned int uMillisecs );
        // One turn of the pump, plus the signal handling P2PeerHub::RunHub
        // does around its own PumpP2Pmsg call. Latches m_bPumpDone on any
        // answer that means "stop". Pump thread only.
        HRESULT
          PumpOnce ( unsigned int uMillisecs, unsigned int *puWhat );

    // P2PeerMsg_MAP handlers
    protected:
      DECLARE_P2PeerMsg_MAP()
        // Wildcard: every topic-named message this hub receives. Kernel-
        // reserved "P2Pmsg*" names are passed on to the P2PeerHub defaults.
        msgRESULT
          On_AnyTopic ( P2PeerMsg *pMsg );
      virtual msgRESULT
        On_P2PeerBCast ( P2PeerMsg *pMsg );
      virtual msgRESULT
        On_P2PeerUCast ( P2PeerMsg *pMsg );
      virtual msgRESULT
        On_P2PeerError ( P2PeerMsg *pMsg );

    // P2PeerCon lifecycle (P2PeerTarget virtuals, dispatched via the base map)
    protected:
      virtual conRESULT
        On_ConLogin    ( P2PeerCon *pCon, P2PaddrSTR strThatP2Paddr
                       , const void *pvLoginMsg, P2Psize_t iSize );
      virtual conRESULT
        On_ConLoginAck ( P2PeerCon *pCon
                       , P2PaddrSTR strThisP2Paddr, P2PaddrSTR strThatP2Paddr
                       , const void *pvLoginAck, P2Psize_t iSize );
      virtual conRESULT
        On_ConClose    ( P2PeerCon *pCon );

    // Timers (P2PeerTarget virtual, dispatched by the pump: P2Pwin32.cpp:4491)
    protected:
      virtual void
        On_PITimer     ( bool bCancel, PITimerID nTimerID, DWORD dwUserKey );

    // Attributes
    private:
        CString                       m_csAddress;   // stable Address() storage
        p2pf::IP2PHubEvents          *m_pEvents;     // client sink (not owned)
        FacadeNetwork                *m_pOwner;      // network (not owned)
        HANDLE                        m_hThread;     // pump thread from SpawnHub
        LONG                          m_bClosed;     // Close() ran (once-only)
        // Peers currently logged in. Written on pump threads (con lifecycle),
        // read by IsPeerUp/Broadcast on any thread -- hence the lock.
        std::map<std::wstring, bool>  m_mapPeers;
        // What this hub ARMED for each peer, canonically spelled, and which
        // way round. The kernel cannot be asked -- every transport keeps its
        // endpoint in protected members with no getters
        // (P2PeerConWsa.h:126-130, P2PeerConPipe.h:108, P2PeerConDmx.h:104,
        // P2PeerCon232.h:106-107) -- so writing it down at arm time is the
        // only way a hub can ever describe itself. Separate from m_mapPeers
        // because that map also grows entries for peers this hub only
        // LEARNED (a wildcard listener adopts the name a dialer claims),
        // which have no endpoint of ours.
        struct ArmedCon
        {
            std::wstring strEndpoint;
            bool         bListen;
        };
        std::map<std::wstring, ArmedCon> m_mapArmed;
        // Peers whose Disconnect signalled the connection but did not see it
        // leave the hub inside the budget. Read by DrainCons, which signals
        // them again at Close.
        //
        // It exists because the maps above cannot express this state: by the
        // time Disconnect gives up it has erased the armed record and marked
        // the peer down, which is indistinguishable from the redialling peer
        // the drain is required to leave alone. Without it a timed-out
        // Disconnect hands Close a hub that still owns a connection -- the
        // kernel teardown the drain exists to prevent.
        std::set<std::wstring>           m_setUnretired;

        // --- ABI 6 ----------------------------------------------------------
        // The optional extended sink. Written by the client thread, read on
        // every pump callback, so it is copied out under the lock and called
        // outside it (ExtEvents).
        p2pf::IP2PHubEvents2         *m_pExtEvents;

        // Timers. The facade hands out its OWN id and keeps the kernel's
        // PITimerID beside it, because SetTimer is called from the client
        // thread and the kernel timer can only be armed later, on the pump --
        // so there is no kernel id to return at the time of the call.
        //
        // The DEADLINE is the facade's, not the kernel's, and it is stamped at
        // the moment of the call rather than at the moment of arming: the
        // client asked for `delay` from when it asked, and the trip through the
        // pump queue is the facade's own latency, not the client's problem.
        // Everything about honouring it -- the compensation in ArmPumpTimer and
        // the re-arm in On_PITimer -- is measured against this one number.
        struct TimerRow
        {
            PITimerID    nKernelID;    // 0 until the pump has armed it
            unsigned int uKey;         // what the client asked to get back
            ULONGLONG    uDeadline;    // GetTickCount64() this must not precede
            unsigned int uRearms;      // early wake-ups already absorbed
        };
        std::map<unsigned int, TimerRow> m_mapTimers;
        unsigned int                  m_uNextTimerId;

        // Outstanding Ping waiters, keyed by request id. The waiter owns the
        // event handle and removes its own row; the pump only ever fills in
        // the result and signals.
        struct PingRow
        {
            HANDLE       hEvent;
            ULONGLONG    uSentTick;
            unsigned int uMillisecs;
            bool         bAnswered;
        };
        std::map<unsigned int, PingRow> m_mapPings;
        unsigned int                  m_uNextPingId;
        // Set by CloseInternal BEFORE it releases the Ping waiters, so a Ping
        // that arrives after that point registers no row to be released --
        // which is what lets the close WAIT for the map to empty and know that
        // no client thread is still inside this object when it returns.
        bool                          m_bClosing;

        mutable CCriticalSection      m_oCSectPeers;   // guards all of the above

        // --- ABI 7 ----------------------------------------------------------
        // The message being delivered RIGHT NOW, so GetMsgInfo can answer
        // about it from inside the client's own handler.
        //
        // Deliberately NOT under m_oCSectPeers, and it needs no lock. Only the
        // pump thread ever writes it, and only the pump thread -- inside the
        // callback, which is the one place it is defined -- ever reads the
        // message pointer. m_lCurMsgThread is the gate and is the ONLY field
        // touched across threads: it is published with an interlocked store
        // AFTER the rest is written and retracted BEFORE the rest is cleared,
        // so a foreign thread either sees an id that is not its own (and stops
        // there, answering P2PF_E_NO_MESSAGE) or nothing at all. There is no
        // arrangement in which it follows the pointer.
        //
        // The pointer is borrowed for exactly the length of one callback: the
        // kernel owns the P2PeerMsg and will have destroyed it long before any
        // later call could ask. That is why there is no "get the message I was
        // given a moment ago" in this ABI, and why the header says to copy.
        P2PeerMsg                    *m_pCurMsg;
        bool                          m_bCurMsgBCast;
        volatile LONG                 m_lCurMsgThread; // 0 == no delivery running

        // --- ABI 9 ----------------------------------------------------------
        // TRUE when this hub was created with P2PF_HUB_CALLER_PUMPED, i.e. it
        // has no thread of its own and runs inside the client's Pump calls.
        // Written once, before the hub is published, and read-only after.
        bool                          m_bCallerPumped;
        // The pump has answered "stop" -- either PumpP2Pmsg returned 0 or it
        // handed back a hub CLOSE signal. Latched, because a client loop that
        // is told to stop must not be told anything else afterwards; the pump
        // it would be driving is gone. Interlocked: Close() sets it from the
        // owning thread, but GetPumpInfo may be asked from any.
        volatile LONG                 m_bPumpDone;
};
