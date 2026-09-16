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
// This is the ONE place in the product where Targetcore's message-map macros
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
                  , FacadeNetwork *pOwner
                  , bool bSecure = false );
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
      // --- ABI 11 ---------------------------------------------------------
      virtual HRESULT GetSecurityInfo ( wchar_t *buf, unsigned int *cch
                                      , unsigned int *outFlags ) const;

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

    // Security -- a property of THIS HUB, settled at construction  -- ABI 11
    //
    // WHY ALL OF IT IS HERE. Every switch behind it is the kernel's and every
    // one of them lives on P2PeerHub -- the identity, the agreement key, the
    // allow-list, the revocation position and the enforcement flags are all
    // read under the hub's own critical section, and an accepted connection is
    // built from a hand-maintained list of copied fields, so a security
    // setting parked on a CONNECTION is one forgotten line away from silently
    // not applying. There is no per-connection override in the kernel and
    // there is none here.
    //
    // WHAT THE NETWORK STILL DOES, and it is one thing: for a Link it holds
    // BOTH hubs, so it can hand each one the other's public points. That is
    // the exchange an operator would otherwise perform with two published
    // files, and it is possible only because there is no wire between them.
    // Everything else -- generating, publishing, trusting, enforcing, gating
    // -- is below.
    public:
        // Created with P2PF_HUB_SECURE?  Fixed for the hub's whole life: a
        // hub that could be secured later would be one whose existing links
        // silently changed terms, and one that could be relaxed later would be
        // one whose secure links silently opened.
        BOOL
          IsSecure ( ) const { return m_bSecure; }

        // The publishable halves of one hub's provisioning, as they go into
        // the OTHER hub's allow-list.
        struct SecKeys
        {
            unsigned char aId    [p2pcng::kEcdsaPubLen];  // ECDSA - proves identity
            unsigned char aAgree [p2pcng::kEcdhPubLen];   // ECDH  - what others seal to
            bool          bAgree;
            char          szFingerprint [p2pcng::kIdFingerprintLen];
        };

        // Give this hub an identity and an agreement key under `csDir`, and
        // keep the two publishable points.  Called by CreateHubEx BEFORE the
        // pump exists, so a hub that cannot be provisioned is never created.
        //
        // IDEMPOTENT, AND THAT IS THE LIBRARY'S ASYMMETRY RATHER THAN A CHOICE
        // MADE HERE: ProvisionAuth run twice FINDS the key rather than
        // replacing it, because a first-run helper that rotated on restart
        // would change a hub's identity behind the operator's back. `rcsWhat`
        // receives a sentence naming the file and the failure when the answer
        // is not IdOk.
        p2pcng::IdResult
          ProvisionSelf ( const CString& csDir, const CString& csRevoke
                        , CString& rcsWhat );
        BOOL
          IsProvisioned ( ) const { return m_bProvisioned; }
        const SecKeys&
          Keys ( ) const { return m_oSecKeys; }

        // Trust one peer and ENFORCE, in that order and as one step.
        //
        // ONE FUNCTION BECAUSE THEY ARE ONE DECISION. Adding a peer to the
        // allow-list is what makes enforcement possible -- the kernel refuses
        // to arm a hub that requires authentication and lists nobody -- so the
        // first TrustAndEnforce on a hub is also the call that switches
        // RequireAuth on, and it must not return success unless the kernel's
        // own arming gate then passes. `rcsWhat` gets the sentence, naming the
        // file, when it does not.
        //
        // THE UNWIND IS CONDITIONAL, and the condition matters: a hub that had
        // no trusted peer yet is put back the way it was spawned, and a hub
        // that is ALREADY carrying authenticated links is left alone --
        // turning its enforcement off to tidy up after a failure would
        // silently open every link it already has.
        HRESULT
          TrustAndEnforce ( const wchar_t *peer, const SecKeys& rKeys
                          , const CString& csDir, CString& rcsWhat );

        // Does this hub already hold `peer` in its trusted set?  Asked by
        // FacadeNetwork::SecureEdgeLocked before it unwinds: trust is not a
        // connection, so a hub can already trust a peer it is being linked to
        // again, and withdrawing that on a failure would take away something
        // this call never granted.
        BOOL
          Trusts ( const wchar_t *peer ) const;

        // Withdraw one peer this hub was told to trust, and rewrite the
        // allow-list without it.  The unwind for a Link whose SECOND hub
        // refused after the first had accepted -- "arms both sides" is a
        // promise about the provisioning too, not only about the two
        // connections.  Enforcement is deliberately left ON: a hub that
        // reaches this has other links, or is about to be told again.
        p2pcng::IdResult
          UntrustPeer ( const wchar_t *peer, const CString& csDir
                      , CString& rcsWhat );

        // Admit a PATTERN peer -- "Demo.*" -- on a secure hub.  A pattern
        // names no peer whose key could be looked up, so the question is not
        // "can I provision this" but "am I already enforcing": the kernel
        // authenticates whoever arrives against the name they CLAIM, and the
        // pattern is only an accept filter over that.  See the definition.
        HRESULT
          AdmitPatternPeer ( const wchar_t *peer );

        // Trust one peer off the two files IT published, and enforce -- the
        // whole of the cross-process provisioning path, and what Listen and
        // Connect do on a secure hub before they arm anything.
        HRESULT
          TrustPublishedPeer ( const wchar_t *peer );

        // The peer's public halves, read from the two files a secure hub
        // publishes for itself: "<peerstem>.key.pub" and, optionally,
        // "<peerstem>.agree.pub".
        //
        // THIS IS THE CROSS-PROCESS PATH and the only one there can be. For a
        // Link the network hands over the peer's keys directly, because it
        // holds both hubs; for Listen/Connect the far end may be anywhere, so
        // the exchange is two files an operator copied -- which is exactly the
        // provisioning step this facade cannot do for them. IdErrNotFound with
        // `rcsWhat` naming the file is the honest answer to "I have no key for
        // that peer", and it is a refusal rather than a plain link.
        static p2pcng::IdResult
          LoadPeerKeys ( const wchar_t *peer, const CString& csDir
                       , SecKeys& rKeys, CString& rcsWhat );

        // The file stem an address's key material is filed under. See the
        // definition for what is done about an address that is not a legal
        // file name.
        static CString
          SecurityStemOf ( const CString& csAddress );
        CString
          SecurityStem ( ) const { return SecurityStemOf ( m_csAddress ); }

    // Internals
    private:
        // Put the hub back the way it was SPAWNED -- enforcement off. The
        // unwind for a hub that TrustAndEnforce turned on and that then would
        // not arm. The keys it holds are left where they are; they are files,
        // they cost nothing, and destroying an identity to unwind one link
        // would change what the hub is the next time it is asked to be secure.
        void
          RelinquishSecurity ( );
        // Put one entry of the trusted set back the way it was FOUND, which is
        // not the same as removing what a call wrote -- see TrustAndEnforce.
        void
          RestoreTrust ( const wchar_t *peer, bool bHadEntry
                       , const SecKeys& rPrev );
        // Which FILE a refusal is about. Sending an operator to the allow-list
        // when the revocation list is the problem is worse than naming nothing:
        // the two live in one directory under similar names.
        CString
          ArmRefusalFile ( p2pauth::ArmResult eArm );
        // Rewrite this hub's allow-list from everything it trusts, and hand it
        // to the kernel. See the definition for why it is a REWRITE.
        p2pcng::IdResult
          WriteAllowList ( const CString& csDir, CString& rcsWhat );
        // One sentence about a security refusal, onto the diagnostic stream as
        // an error. P2PF_E_SECURITY is one code covering a family of file
        // problems, and a code with no sentence sends an operator through a
        // directory by hand.
        void
          RaiseSecurityError ( const wchar_t *what );

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
        // The security posture EVERY hub is spawned with, secure or not,
        // applied before the pump exists because that is the only point at
        // which the kernel reads it as a gate.  See the definition -- a secure
        // hub differs only in holding keys, and it turns enforcement on later,
        // when it has a peer to enforce against.              (ABI 11)
        void
          ApplyDefaultPosture ( );

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

        // --- ABI 11 ---------------------------------------------------------
        // Whether this hub was created with P2PF_HUB_SECURE, and the material
        // it was given if it was.
        //
        // m_bSecure is written once, by the constructor, and read without a
        // lock everywhere after -- it cannot change, so there is nothing to
        // serialise. The rest is under m_oCSectPeers like the maps above it,
        // and for the same reason: a client thread adds a trusted peer while a
        // pump thread is inside a login that is enforcing the list. The KEYS
        // themselves are not here -- they are the kernel's, held by the hub's
        // AuthPolicy; what is kept is the PUBLIC halves, because a peer's
        // allow-list needs them and the kernel offers no way to read another
        // hub's back out.
        const bool                    m_bSecure;
        bool                          m_bProvisioned;
        SecKeys                       m_oSecKeys;
        // Every peer this hub has been told to trust, and with what. The
        // allow-list FILE is rewritten from this on every addition -- see
        // WriteAllowList for why it is a rewrite and not an append.
        std::map<std::wstring, SecKeys> m_mapTrusted;
        // The directory this hub's key material came from, kept because the
        // arming verbs need it after CreateHubEx resolved it once.
        CString                       m_csSecDir;
};
