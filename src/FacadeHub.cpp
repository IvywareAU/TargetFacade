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
// FacadeHub.cpp -- IP2PHub over P2PeerHub.
#include "stdafx.h"
#include "FacadeHub.h"
#include "FacadeInternal.h"
#include "FacadeMessage.h"

#include <vector>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace {

// "P2Pmsg*" is the kernel's reserved message-name namespace (P2PmsgBCast,
// P2PmsgException, ...). Clients may not send into it; on receive those names
// are handed back to the P2PeerHub default handlers.
inline bool
IsReservedName ( const wchar_t *lpszName )
{
    return lpszName && ::wcsncmp ( lpszName, L"P2Pmsg", 6 ) == 0;
}

// ---------------------------------------------------------------------------
// The facade's OWN reserved namespace, "P2PF$"                        (ABI 6)
//
// Three of the ABI 6 additions -- SetTimer, Post and Ping -- are calls made on
// a CLIENT thread whose work has to happen on the PUMP thread. The kernel has
// exactly one ordered way in, and the facade already uses it for every message
// it sends: post a P2PeerMsg. So each of them posts one, under a private
// topic, and the receive path picks it out before the client sink is reached.
//
// Marshalling this way rather than by locking is the whole point: the work
// lands in FIFO order with the hub's own traffic, on the thread that owns the
// hub's state, which is precisely what the kernel's P2PmsgSink does and what a
// client cannot build for itself from the ABI 5 surface.
//
// Two of the five are accepted FROM ANOTHER HUB (Ping/Pong -- they have to be,
// that is the round trip). The other three are this hub talking to its own
// pump, and HandlePrivate refuses them unless the source is this hub's own
// address: a remote peer must never be able to arm a timer or run a Post here.
// Send/Broadcast reject the whole prefix, so a client cannot manufacture one.
// ---------------------------------------------------------------------------
const wchar_t kPrivPrefix[] = L"P2PF$";
const wchar_t kTopicTimer[] = L"P2PF$Timer";
const wchar_t kTopicKill [] = L"P2PF$Kill";
const wchar_t kTopicPost [] = L"P2PF$Post";
const wchar_t kTopicPing [] = L"P2PF$Ping";
const wchar_t kTopicPong [] = L"P2PF$Pong";

inline bool
IsPrivateName ( const wchar_t *lpszName )
{
    return lpszName
        && ::wcsncmp ( lpszName, kPrivPrefix, _countof(kPrivPrefix) - 1 ) == 0;
}

#pragma pack(push,1)
// The id, and nothing else: the delay, the key and the deadline all live in
// the TimerRow the caller wrote before posting this, which is the single place
// they are read from. A body that repeated them could disagree with it.
struct PrivTimerBody { UINT32 uTimerId; };
struct PrivKillBody  { UINT32 uTimerId; };
struct PrivPostBody  { UINT32 uKey; UINT64 uContext; };
struct PrivPingBody  { UINT32 uPingId; };
#pragma pack(pop)

// ---------------------------------------------------------------------------
// P2PF_SEND_NO_BOUNCE, and the kernel toggle that is not there          (ABI 7)
//
// P2PeerMsg has a control byte for exactly this -- P2PeerMsgPrefix::uiCtrl,
// with P2PeerMsgCtrl_EXCEPTIONS (P2PeerMsg.h:66-72) -- and four separate
// comments in P2PeerTarget.cpp (1526, 1568, 1619, 1649) tell the reader that
// "individual P2PeerMsg exceptions can be toggled on and off via the
// P2PeerMsg::Exceptions() method".
//
// BOTH OVERLOADS OF THAT METHOD ARE DECLARED (P2PeerMsg.h:257-260) AND
// IMPLEMENTED NOWHERE -- the class is dllimport, so calling either compiles
// and then fails to link, which is how this was found. P2PeerMsgCtrl_EXCEPTIONS
// itself is never consulted anywhere: it appears in its own definition and in
// P2PeerMsgCtrl_DEFAULT, which is unused. The bit is inert and the toggle does
// not exist. Same family as WakeupHub's ASSERT(0) stub and the P2Pmsg_Error
// nobody posts: a documented capability that measurement says is absent.
//
// uiCtrl IS read in exactly one place, and it matters here for one reason only.
// P2PeerTarget::NotHandled ends with
//
//     BOOL bDisplay = P2PeerMsg_SetCtrlOptions(pMsg,0,0)&P2PeerMsgCtrl_HANDLED;
//
// which is dead twice over -- the mask 0 zeroes the byte before returning it,
// so bDisplay is always FALSE, and P2PeerMsgCtrl_HANDLED is never set by
// anything -- but it does WIPE uiCtrl as a side effect. Harmless to this code,
// because both reads below happen strictly BEFORE NotHandled runs, and on the
// only path where it runs at all the bit was clear anyway. Anyone reading the
// bit later in the pipeline than that will find it gone.
//
// So the facade carries the intent itself, in that same byte, through the one
// accessor the kernel does export (P2PeerMsg_SetCtrlOptions, whose second
// parameter is an AND MASK rather than the "bits to remove" its name promises).
//
// POLARITY IS THE WHOLE DESIGN. A SET bit means SUPPRESS. So zero -- which is
// what every message from a non-facade peer carries, and every message any
// ABI 5 or 6 client ever sent -- means "report", i.e. exactly today's
// behaviour. Had the bit meant "report", every one of those would have gone
// quiet the day this shipped.
//
// The top bit, not P2PeerMsgCtrl_EXCEPTIONS itself: squatting on a kernel-named
// constant with the opposite meaning is how the next reader gets it wrong.
const UINT08 kCtrlNoBounce = 0x80;

// Read the control byte without changing it: add nothing, mask everything.
inline UINT08
CtrlOf ( P2PeerMsg *pMsg )
{
    try { return P2PeerMsg_SetCtrlOptions ( pMsg, 0, 0xFF ); }
    catch ( ... ) { return 0; }
}

inline bool
IsNoBounce ( P2PeerMsg *pMsg )
{
    return pMsg && ( CtrlOf ( pMsg ) & kCtrlNoBounce ) != 0;
}

// ---------------------------------------------------------------------------
// Named fields, and where they live on the wire                       (ABI 8)
//
// A P2PeerMsg is a P3PmsgBSTR: a named tree whose root item has one child per
// "slot". The kernel's own are Net (addressing), Msg (the application payload),
// Wrp (a wrapped message) and Evt (an attached event); Data()/DataSize() -- and
// therefore everything every existing peer reads -- are the blob on Msg.
//
// The facade adds ONE MORE CHILD OF THE ROOT, in its own "P2PF$" namespace,
// holding one item per field. Three reasons this shape and not another:
//
//   * Msg IS UNTOUCHED. A peer that is not this facade, or is an older one,
//     reads exactly the payload it always read and never learns the fields are
//     there. Adding fields to a message cannot break a receiver -- which is
//     what ruled out the obvious alternative of framing the fields into the
//     payload the way Broadcast frames its topic.
//   * IT IS NAMED, NOT NUMBERED. P2PmsgBSTR.h reserves three spare slots
//     (VBLockBSTR_SP4/SP5/SP6) and it is tempting to take one; they are not
//     wired into P3PmsgBSTR::r_item at all (it handles ROOT/NET/MSG/SYS/WRP/EVT
//     and returns SelectItem(L"?") for anything else), and a future kernel that
//     claimed one would collide with this on the wire.
//   * IT TRAVELS. P3PmsgBSTR::PrepareP2Piomage takes the "optimised full
//     replica" path when the mask is all-ones and ships the whole VBList
//     verbatim, and P2Peerio's mask is ~0 by default. So every slot crosses,
//     including this one -- measured over TCP in section 25, because "the
//     default mask is all-ones" is an argument and the wire is a fact. The
//     corollary: a client that narrows the mask with P2Peerio::SetIFmask
//     through GetNative would drop these.
//
// READ THE RIGHT HEADER. There are two P2Pmsg headers in this tree and the
// interesting one is not the one that compiles: `#include "P2Pmsg.h"` resolves
// to ..\Msgcore\P2Pmsg.h, NOT to ..\TargetCore\P2Pmsg(2Msgcore).h. In the file
// that actually builds, class P3PmsgNode -- the whole thing, ~130 lines,
// AddNode/DeclareNode/SelectNode/GetCount/r_Curs -- IS COMMENTED OUT, and
// `typedef P3PmsgField P3PmsgItem` is what remains. So the tree is items with
// named children and nothing else: SelectItem, DeclareItem, Exists, Delete.
//
// WHICH IS WHY THE FACADE WRITES ITS OWN INDEX. P3PmsgField has no child count
// and no cursor -- P3PmsgCurs is forward-declared in that header and never
// defined -- so a receiver can look a field up BY NAME and cannot discover what
// names are there. GetFieldCount/GetFieldName would be unimplementable. The
// index item below carries the names, tab-separated, in insertion order, which
// is exactly what IP2PMessage::GetFieldName promises. It costs a few bytes per
// message and is the same move BCastFrame already makes for a broadcast topic.
const wchar_t kFieldsItem[] = L"P2PF$Fields";
const wchar_t kNamesItem [] = L"P2PF$Names";

// Ping budget: 0 means "the sensible default", and there is a ceiling because
// this call blocks a client thread and a caller that asks for an hour has
// almost certainly made an arithmetic mistake.
const unsigned int kPingDefaultMillisecs =  5000;
const unsigned int kPingMaxMillisecs     = 60000;

// ---------------------------------------------------------------------------
// Timers that do not fire early.
//
// The kernel's deadline is `_time64(0)*1000 + delay` (P2Pwin32.cpp:5321) and
// its poll is `deadline - _time64(0)*1000` (P2Pwin32.cpp:515). BOTH read a
// clock with one-second granularity, so both mean "the top of the current
// second", and the timer elapses at the first pump WAKE-UP whose second has
// reached that deadline.
//
// The two errors cancel exactly when the pump's next wake-up is the timer's
// own: the poll overstates what is left by the same `f` ms into the second
// that the arm understated the deadline by, so the sleep ends where the caller
// asked. That is the ordinary case and it is why this was never noticed.
//
// It stops cancelling the moment ANYTHING ELSE wakes the pump first -- a
// message, a connection event, another timer -- because that wake-up polls the
// same truncated clock without having slept for it. Then the deadline reads as
// already past and the timer elapses early: measured, a 200 ms timer armed
// 5 ms before a second boundary fired in 5 ms. On a hub with traffic, which is
// every hub that is doing anything, that is the common case rather than the
// odd one.
//
// So the facade keeps its own deadline and refuses to deliver before it,
// re-arming instead (see On_PITimer). The re-arm ADDS `f` back, which puts the
// kernel's deadline at the facade's own and cannot be early whatever wakes the
// pump -- so one re-arm always settles it, and the first arm is left alone to
// stay exact in the quiet case.
//
// FILETIME rather than the CRT clock because _time64 reads the same system
// clock and the two share second boundaries -- this is the position WITHIN the
// second that _time64 is about to truncate away.
inline unsigned int
MillisecsIntoSecond ( )
{
    FILETIME ft;
    ::GetSystemTimeAsFileTime ( &ft );
    ULONGLONG uTicks = ( (ULONGLONG)ft.dwHighDateTime << 32 )
                     |   (ULONGLONG)ft.dwLowDateTime;
    return (unsigned int)( ( uTicks / 10000ULL ) % 1000ULL );
}

// One compensated re-arm settles it; the spare is for a clock that has been
// stepped under the hub (the deadline is a tick count and the compensation is
// system time, and nothing obliges the two to agree across an NTP correction).
// Bounded because a disagreeing clock must delay a callback, never cancel it.
const unsigned int kTimerMaxRearms = 2;

// Close budget for outstanding Ping waiters. They are released by SetEvent
// before this wait begins, so it is normally over in one slice; the budget is
// only there so a close can never hang on a thread that is not coming back.
const int kPingDrainSlices = 40;   // 1 s at kDrainSliceMillisecs

// ---------------------------------------------------------------------------
// Broadcast body framing.
//
// A broadcast MUST travel under the kernel name P2Pmsg_BCast (that is what
// makes P2PeerHub::On_P2PeerBCast relay it onward to child hubs), so the
// client's topic cannot be the message name the way it is for a unicast --
// it has to ride inside the body. The frame keeps that binary-safe:
//
//     [UINT16 magic][UINT16 topicChars][topic UTF-16, no NUL][payload bytes]
//
// A body that does not carry the magic is treated as an unframed broadcast
// from a non-facade peer (the plain-text form the TargetCore samples send):
// it is delivered whole, under the kernel message name.
// ---------------------------------------------------------------------------
#pragma pack(push,1)
struct BCastFrame
{
    UINT16 uMagic;
    UINT16 uTopicChars;
};
#pragma pack(pop)

const UINT16 kBCastMagic = 0xF2CA;

// P2PeerMsg32 rejects a null data pointer; a zero-length payload still needs
// somewhere to point.
const char kEmptyPayload[1] = { 0 };

// Connection drain budget on Close (see FacadeHub::DrainCons).  Generous:
// overshooting costs a slow close, undershooting costs the crash the drain
// exists to prevent.
const int kDrainSliceMillisecs =   25;
const int kDrainSlices         =  120;   // 3 s

// CloseCon's two budgets are FacadeHub::kUnwindSlices and kRetireSlices, in
// the header beside the method whose contract they are.  Same slice unit as
// the drain above.

// ---------------------------------------------------------------------------
// Retrying dial connections.
//
// A raw ClientFactory connection dials ONCE. That makes the whole facade
// order-dependent: Connect must not run before the far side's Listen has
// actually armed, which the client cannot observe (arming happens later, on
// the peer hub's pump thread). Measured: back-to-back listenPipe/connectPipe
// fails in a Release build and survives in Debug purely on timing.
//
// So every facade dial is armed with the kernel's own auto-restart instead:
// m_uAutoRestart makes the default On_ConClose handler Restart() the
// connection on a paced timer after each close, which covers both "the peer
// is not up yet" and "an established link dropped".
//
// HasDroppedOut is what decides whether a failed attempt is routine or
// shouted about: P2PeerCon::OnClose puts the precipitating P2Pevent in a
// MODAL MESSAGE BOX unless it returns true. A library must never do that --
// one box per retry would wedge a headless process -- so a facade dial calls
// every failure routine and reports state through OnPeerDown/OnError instead.
// (Blanket true rather than an errno whitelist on purpose: the kernel reads
// GetLastError() after its own message formatting, so the code that reaches
// here is not always the one the OS set.)
// ---------------------------------------------------------------------------
const P2Pmsecs_t kRedialMillisecs = 500;

class RetryDialWsa : public P2PeerConWsa
{
    public:
      static RetryDialWsa*
        Make ( P2PaddrSTR strThat, LPCTSTR lpszIp, short nPort )
      {
          // The 3-arg base ctor sets peer address, IP, port and CLIENT mode;
          // the protocol object is the factory's only other job.
          RetryDialWsa *pCon = new RetryDialWsa ( strThat, lpszIp, nPort );
          pCon->SetP2Peerio ( new P2Peerio() );
          pCon->m_uAutoRestart = kRedialMillisecs;
          return pCon;
      }
      virtual bool HasDroppedOut ( HRESULT ) { return true; }

    private:
        RetryDialWsa ( P2PaddrSTR strThat, LPCTSTR lpszIp, short nPort )
          : P2PeerConWsa ( strThat, lpszIp, nPort ) { }
};

class RetryDialPipe : public P2PeerConPipe
{
    public:
      static RetryDialPipe*
        Make ( P2PaddrSTR strThat, LPCTSTR lpszPipename )
      {
          RetryDialPipe *pCon = new RetryDialPipe ( strThat, lpszPipename );
          pCon->m_eP2PeerConMode = P2PeerCon_CLIENT;  // ctor does not set it
          pCon->SetP2Peerio ( new P2Peerio() );
          pCon->m_uAutoRestart = kRedialMillisecs;
          return pCon;
      }
      virtual bool HasDroppedOut ( HRESULT ) { return true; }

    private:
        RetryDialPipe ( P2PaddrSTR strThat, LPCTSTR lpszPipename )
          : P2PeerConPipe ( strThat, lpszPipename ) { }
};

// ---------------------------------------------------------------------------
// Every Dmx connection the facade manufactures -- and the accept filter the
// kernel's own Dmx factories forget.
//
// `toPeer` is TWO stamps on a connection: m_oThatP2Paddr, the address it
// routes to, and m_oP2Padomain, the filter an arriving login's CLAIMED address
// is checked against (P2PeerCon.cpp:1876-1886). The second is the only
// built-in place a topology error is caught at LOGIN rather than diagnosed
// later from a message that quietly failed to route.
//
// It was not applied uniformly. P2PeerCon's (addr, io) ctor sets both, and
// P2PeerConWsa/Pipe/232's factories set m_oP2Padomain again explicitly
// (P2PeerConWsa.cpp:111, P2PeerConPipe.cpp:80, P2PeerCon232.cpp:103) -- but
// P2PeerConDmx::ServiceFactory and ClientFactory build from the DEFAULT ctor
// and assign only m_oThatP2Paddr, so the domain stays null. The kernel's guard
// reads a null domain as "no restriction" and says so in its own comment at
// P2PeerCon.cpp:1871.
//
// So the same listen(peer, ...) enforced the peer's claimed identity over
// three transports and not over the fourth -- and the fourth is what an
// OMITTED endpoint resolves to, which is the path this facade recommends.
// Measured before the fix: a hub addressed Flt.Wrong dialled a service armed
// for Flt.Right and logged in; the identical shape over tcp was refused.
//
// The list is not exported and the member is protected, so this cannot be
// fixed from outside a subclass -- which is why the facade builds its own Dmx
// connections rather than calling the kernel factories. Everything else about
// them is ServiceFactory/ClientFactory field for field, including the
// P2PeerioDmx that only those factories ever supplied.
// ---------------------------------------------------------------------------
class FacadeConDmx : public P2PeerConDmx
{
    public:
      static P2PeerCon*
        Make ( P2PaddrSTR strThat, LPCTSTR lpszService, bool bListen )
      {
          FacadeConDmx *pCon = new FacadeConDmx ( );
          pCon->Stamp ( strThat, lpszService
                      , bListen ? P2PeerCon_SERVICE : P2PeerCon_CLIENT );
          return pCon;
      }

    protected:
      void Stamp ( P2PaddrSTR strThat, LPCTSTR lpszService
                 , P2PeerConMode_e eMode )
      {
          m_oThatP2Paddr   = strThat;
          m_oP2Padomain    = strThat;   // the stamp the kernel factories omit
          m_sServiceName   = lpszService;
          m_eP2PeerConMode = eMode;
          SetP2Peerio ( new P2PeerioDmx ( ) );
      }
};

// ---------------------------------------------------------------------------
// The DERIVED Dmx dial -- retrying, but on a budget.
//
// A dmx:// endpoint a caller TYPED still dials once and fails: a named
// in-process service that is not there is a configuration fault, and no amount
// of redialling conjures one. A DERIVED endpoint is a different animal.
// ResolveDial has already refused unless a hub of that address is live in this
// process AND has recorded a listener expecting us, so "not there" is off the
// table before MakeCon runs and the only failure left is timing -- and timing
// is what the raw ClientFactory handles worst:
//
//   - ArmRecorded writes the listener's record BEFORE posting its connection
//     (deliberately, see the notes there), and the arming itself happens later
//     on that hub's pump thread. So a resolution can hand back a good service
//     name for a service that does not exist YET.
//   - The kernel's rendezvous spin is far shorter than it looks. The budget is
//     `_time64(0)*1000 + 100` with one-second-granularity _time64
//     (P2PeerConDmx.cpp:682, 727-729), so where the call lands relative to a
//     second boundary decides everything: anywhere from ~1000 ms down to
//     nearly nothing.
//   - The loss is SILENT. Connect returned S_OK long before the throw and the
//     peer never came up, so no OnPeerDown fires either. Nothing at all
//     reaches the client.
//
// BOUNDED, unlike the two wrappers above, and for a reason specific to this
// transport: a failed Dmx attempt is not free the way a failed TCP connect is
// -- the spin blocks the DIALING hub's pump thread for its whole duration.
// Redialling forever against a service that has gone away (the peer hub
// closed, which is ordinary) would stall that pump more than half the time,
// permanently, with nothing said about it.
//
// Eight attempts, paced at kRedialMillisecs, is far past any plausible lag in
// the thing actually being waited for: one P2P_Listen dequeue on a sibling
// pump. The budget is spent BEFORE delegating because the base call throws out
// of the frame on a miss and never comes back, and it is REFRESHED on success
// so this stays a rendezvous budget and not a lifetime one -- a link that runs
// for a week and then drops gets a full eight attempts to re-establish, which
// is the reconnect behaviour tcp and pipe already have.
// ---------------------------------------------------------------------------
const int kRedialDmxTries = 8;

class RetryDialDmx : public FacadeConDmx
{
    public:
      static RetryDialDmx*
        Make ( P2PaddrSTR strThat, LPCTSTR lpszService )
      {
          RetryDialDmx *pCon = new RetryDialDmx ( );
          pCon->Stamp ( strThat, lpszService, P2PeerCon_CLIENT );
          pCon->m_uAutoRestart = kRedialMillisecs;
          return pCon;
      }
      virtual bool HasDroppedOut ( HRESULT ) { return true; }

      // TRUE once the budget is spent. Read by FacadeHub::On_ConClose, which
      // is the one place that can turn "stopped trying" into a client event.
      bool    Exhausted   ( ) const { return m_nTriesLeft <= 0; }
      LPCTSTR ServiceName ( ) const { return (LPCTSTR)m_sServiceName; }

      virtual bool Connect ( )
      {
          if ( --m_nTriesLeft <= 0 )
            m_uAutoRestart = 0;        // make THIS attempt the last one

          bool bDone = P2PeerConDmx::Connect ( );   // throws on a miss

          m_nTriesLeft   = kRedialDmxTries;
          m_uAutoRestart = kRedialMillisecs;
          return bDone;
      }

    private:
        RetryDialDmx ( ) : m_nTriesLeft ( kRedialDmxTries ) { }
        int m_nTriesLeft;
};

} // namespace

// ---------------------------------------------------------------------------
// The ONE message map in the product.
//
// Plain ON_P2PeerMsg, not ON_P2PeerMsg_PEEK: PEEK's ~0 state filter matches
// REFLECTED messages only. Reserved names return msgCONTINUE from the handler
// and so travel on down the base-map chain to the P2PeerHub defaults.
// ---------------------------------------------------------------------------
BEGIN_P2PeerMsg_MAP(FacadeHub, P2PeerHub)
    ON_P2PeerMsg(_N("*"), &FacadeHub::On_AnyTopic)
END_P2PeerMsg_MAP()

///////////////////////////////////////////////////////////////////////
//  Construction

FacadeHub::FacadeHub ( P2PaddrSTR strAddress
                     , p2pf::IP2PHubEvents *pEvents
                     , FacadeNetwork *pOwner )
        : P2PeerHub   ( strAddress )
        , m_csAddress ( strAddress )
        , m_pEvents   ( pEvents )
        , m_pOwner    ( pOwner )
        , m_hThread   ( 0 )
        , m_bClosed   ( 0 )
        , m_pExtEvents( 0 )
        , m_uNextTimerId ( 0 )
        , m_uNextPingId  ( 0 )
        , m_bClosing     ( false )
        , m_pCurMsg      ( 0 )
        , m_bCurMsgBCast ( false )
        , m_lCurMsgThread( 0 )
        , m_bCallerPumped( false )
        , m_bPumpDone    ( 0 )
{
}

FacadeHub::~FacadeHub ( )
{
    CloseInternal();
}

BOOL
FacadeHub::Start ( )
{
    m_hThread = SpawnHub();
    return m_hThread != 0;
}

//
//  Start with no thread of our own                                    (ABI 9)
//  NOTES: P2PeerHub::CreateHub is the kernel's own answer to this and has been
//         there all along -- "Hub is created and run in context of calling
//         thread ... Client is responsible for pumping messages through the
//         P2PmsgHub" (P2PeerHub.cpp:208-221).  The facade has simply never
//         called it
//       : m_hThread stays 0, and that is what every other method reads to tell
//         the two kinds of hub apart on the teardown path.  It is not merely
//         unset -- there is no handle to hold, and CloseInternal must not wait
//         on one
//       : THROWS, unlike SpawnHub.  CreateP2PmsgHub reports every one of its
//         refusals by throwing a P2Pevent, and the one a caller will actually
//         meet is "P2PmsgPump already exists in contect of thread" -- one pump
//         per thread is the kernel's rule (P2Pwin32.cpp:1741-1747), so a
//         thread that already drives a caller-pumped hub, or that IS some
//         other hub's pump, cannot have a second.  Caught here and reported as
//         a plain FALSE, which CreateHubEx turns into P2PF_E_HUB_SPAWN
//       : Sets the pump's diagnostic name the way RunHub does, so a hub that
//         is caller-pumped says so in a kernel trace rather than looking like
//         a pump nobody named
//
BOOL
FacadeHub::StartCallerPumped ( )
{
    try
    {
      if ( !P2PeerHub::CreateHub ( (LPCTSTR)m_csAddress, 1 ) )
        return FALSE;
      SetP2PmsgPumpFunc ( 0, _T("FacadeHub::Pump") );
    }
    catch ( P2Pevent *pEVT ) { pEVT->Cancel(); return FALSE; }
    catch ( ... )            {                return FALSE; }

    m_bCallerPumped = true;
    return TRUE;
}

///////////////////////////////////////////////////////////////////////
//  ABI 9 -- whose thread the pump runs on
//
//  A hub's pump IS a thread: the kernel keys a pump by the id of the thread it
//  was constructed on and refuses to give one thread two (the P2PmsgPump
//  constructor stamps m_nPumpID = GetCurrentThreadId(), P2Pwin32.cpp:211-213,
//  and both CreateP2PmsgHub and CreateP2PmsgPump open by throwing if the
//  calling thread already has one).  That single fact decides everything here:
//  it is why a caller-pumped hub belongs to exactly one thread, why multi-pump
//  would mean multi-THREAD, and why GetHubID() is a complete answer to "who
//  runs this hub" for both kinds.

bool
FacadeHub::OwnsPump ( ) const
{
    P2PmsgHubID nHubID = GetHubID();
    return nHubID != 0 && ::GetCurrentThreadId() == nHubID;
}

//
//  Wait, or work
//  NOTES: THE one line that makes a caller-pumped hub a first-class hub.  Every
//         wait in this file is a wait for the PUMP to do something -- a
//         connection to leave the list, a ping answer to arrive, a client
//         thread to leave a frame.  On a spawned hub the pump is elsewhere and
//         sleeping through the slice is exactly right.  On a caller-pumped hub
//         owned by this very thread, sleeping through the slice guarantees the
//         thing being waited for never happens, so the wait has to BE the work
//       : This is also why Disconnect and Ping stop refusing here.  Their
//         P2PF_E_PUMP_THREAD guard exists because blocking the pump thread
//         blocks the only thread that could satisfy the wait -- and that is
//         precisely what this removes
//       : m_bPumpDone is honoured so a wait cannot spin against a pump that has
//         already been told to stop.  Past that point there is nothing left to
//         drive and the caller's budget is better spent expiring
//
void
FacadeHub::WaitSlice ( unsigned int uMillisecs )
{
    if ( !m_bCallerPumped || !OwnsPump() || ::InterlockedCompareExchange (
             const_cast<volatile LONG*>(&m_bPumpDone), 0, 0 ) )
    {
      ::Sleep ( uMillisecs );
      return;
    }
    PumpOnce ( uMillisecs, 0 );
}

//
//  One turn of the pump, with the signal handling RunHub does around its own
//  NOTES: P2PeerHub::RunHub is the model and is followed rather than
//         paraphrased (P2PeerHub.cpp:366-419): pump once, then read the signal
//         it handed back.  The CLOSE and CLOSEONIDLE arms are copied because
//         they are not optional -- a hub that is told to close must pass
//         DESTROY to its connections and CLOSE to its pumps, and a facade that
//         skipped that would leave exactly the unretired connections DrainCons
//         exists to prevent
//       : PAUSE and WAKEUP are NOT copied, and that is deliberate.  RunHub
//         answers P2PsigHub_WAKEUP by calling WakeupHub, whose hub-thread
//         branch is an ASSERT(0) stub (P2PeerHub.cpp:248-262) -- so a client
//         driving its own loop would fire a debug assertion in its own face
//         for a signal the facade never sends.  PAUSE is reached instead
//         through CloseIdleCons, which performs it directly
//       : Returns 0 from PumpP2Pmsg meaning STOP, which is what RunHub's loop
//         condition tests.  Latched into m_bPumpDone, because a client asking
//         again after that would be driving a pump that is being torn down
//       : Exceptions are swallowed to a stop rather than let out.  A client
//         loop cannot be asked to catch a P2Pevent* -- the facade's whole
//         premise is that no kernel type crosses the boundary -- and RunHub's
//         own answer to the same exception is to end the loop
//
HRESULT
FacadeHub::PumpOnce ( unsigned int uMillisecs, unsigned int *puWhat )
{
    DWORD    dwResult = 0;
    P2PsigID nSigID   = 0;

    try
    {
      dwResult = PumpP2Pmsg ( uMillisecs, nSigID );
    }
    catch ( P2Pevent *pEVT )
    {
      pEVT->Advice ( _T("P2PeerHub(%s) P2PmsgHub terminated")
                   , (LPCTSTR)m_csAddress )->Cancel();
      ::InterlockedExchange ( &m_bPumpDone, 1 );
      return p2pf::P2PF_E_CLOSED;
    }
    catch ( ... )
    {
      ::InterlockedExchange ( &m_bPumpDone, 1 );
      return p2pf::P2PF_E_CLOSED;
    }

    if ( puWhat )
      *puWhat = (unsigned int)dwResult;

    // RunHub's loop condition: 0 means the pump is finished.
    if ( dwResult == 0 )
    {
      ::InterlockedExchange ( &m_bPumpDone, 1 );
      return p2pf::P2PF_E_CLOSED;
    }

    if ( nSigID == P2PsigHub_CLOSE || nSigID == P2PsigHub_CLOSEONIDLE )
    {
      P2PsigID nConSig = ( nSigID == P2PsigHub_CLOSE )
                       ? P2PsigCon_DESTROY : P2PsigCon_CLOSEONIDLE;
      P2PsigID nPmpSig = ( nSigID == P2PsigHub_CLOSE )
                       ? P2PsigPump_CLOSE : P2PsigPump_CLOSEONIDLE;
      try
      {
        P2PeerCon *pCon = 0;
        while ( EnumP2PmsgCon ( GetHubID(), &pCon ) )
          pCon -> Signal ( nConSig );

        P2PumpID nPumpID = 0;
        while ( EnumP2PmsgPump ( GetHubID(), nPumpID ) )
          SignalP2PmsgPump ( nPumpID, nPmpSig );
      }
      catch ( ... ) { }

      ::InterlockedExchange ( &m_bPumpDone, 1 );
      return p2pf::P2PF_E_CLOSED;
    }

    return ( dwResult == P2PmsgPump_TIMEOUT ) ? S_FALSE : S_OK;
}

//
//  Run one turn of this hub's pump on the caller's thread
//  NOTES: Two refusals, and they are different mistakes told apart on purpose.
//         P2PF_E_NOT_PUMPED is "this hub has a thread of its own, and driving
//         it from here would mean two threads inside one pump"; P2PF_E_PUMP_OWNER
//         is "right kind of hub, wrong thread".  One is a design error at the
//         call site, the other a threading error, and a single code for both
//         would send a reader looking in the wrong place
//       : The owner check is against GetHubID(), which for a caller-pumped hub
//         is the id of the thread CreateP2PmsgHub ran on -- so it is not
//         bookkeeping the facade could get out of step with, it is the kernel's
//         own answer to the same question
//
HRESULT
FacadeHub::Pump ( unsigned int timeoutMillisecs, unsigned int *outWhat )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( outWhat )
      *outWhat = 0;

    if ( !m_bCallerPumped )
      return p2pf::P2PF_E_NOT_PUMPED;
    if ( !OwnsPump() )
      return p2pf::P2PF_E_PUMP_OWNER;
    if ( ::InterlockedCompareExchange ( &m_bPumpDone, 0, 0 ) )
      return p2pf::P2PF_E_CLOSED;

    return PumpOnce ( timeoutMillisecs, outWhat );
}

//
//  How much is queued and not yet dispatched
//  NOTES: The pump's own message FIFO and nothing else.  Connection activity
//         and completed transport operations arrive through the completion
//         port, which has no count to give -- so 0 says "the FIFO is empty",
//         never "Pump would find nothing", and the header says so
//       : Answerable on a spawned hub too, and from any thread, because
//         GetP2PmsgCount takes the pump id rather than assuming the caller's
//
HRESULT
FacadeHub::GetPending ( unsigned int *outCount ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outCount ) return E_POINTER;
    *outCount = 0;

    P2PmsgHubID nHubID = GetHubID();
    if ( !nHubID )
      return p2pf::P2PF_E_CLOSED;

    try { *outCount = (unsigned int)GetP2PmsgCount ( nHubID ); }
    catch ( ... ) { return p2pf::P2PF_E_CLOSED; }
    return S_OK;
}

HRESULT
FacadeHub::GetPumpInfo ( unsigned int *outFlags, unsigned int *outThreadId ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outFlags ) return E_POINTER;

    P2PmsgHubID nHubID = GetHubID();
    unsigned int uFlags = 0;
    if ( m_bCallerPumped )
      uFlags |= p2pf::P2PF_PUMP_CALLER_DRIVEN;
    if ( nHubID != 0 && ::GetCurrentThreadId() == nHubID )
      uFlags |= p2pf::P2PF_PUMP_THIS_THREAD;

    *outFlags = uFlags;
    if ( outThreadId )
      *outThreadId = (unsigned int)nHubID;
    return S_OK;
}

//
//  Retire this hub's connections BEFORE the hub itself is closed
//  NOTES: RunHub answers P2PsigHub_CLOSE by posting P2PsigCon_DESTROY to
//         every connection and then BREAKING out of the pump loop
//         (P2PeerHub.cpp:386-396) -- so the DESTROYs it just queued are
//         never pumped.  Teardown then falls through to CloseP2PmsgHub's
//         sweep, which walks m_oCListP2PmsgCon calling Destroy()+Drop(0) on
//         each con while that same Drop can delete list entries underneath
//         the walk (P2Pwin32.cpp:2288-2294; the code's own notes at :2296-
//         2302 spell out the DropP2PmsgCon deletion path).  Closing a hub
//         that still owns connections therefore corrupts the heap: measured
//         as an access violation on one run and a fail-fast on the next,
//         at a different connection each time
//       : So retire them while the pump is STILL RUNNING and can actually
//         process the signal.  Post DESTROY to every connection and wait
//         for the list to drain; by the time the hub is signalled to close
//         there is nothing left for the sweep to walk
//       : THREE classes of peer are signalled, and a fourth deliberately is
//         not.  Never "conID 0 == all connections" either way
//           - LOGGED IN (m_mapPeers[p] == true).  Established connections are
//             what a listener spawns accepted clones for, so they are the
//             ones the sweep is most likely to trip over
//           - ARMED BUT NEVER UP (in m_mapArmed, no m_mapPeers entry at all).
//             MarkPeer only ever writes on login/close, so "no entry" is
//             exactly "this connection has never connected".  These MUST be
//             retired too: an endpoint-less listen() is a Dmx SERVICE by
//             construction, and P2PeerConDmx::Listen registers it in the
//             PROCESS-GLOBAL g_oCListP2PeerConDmx (P2PeerConDmx.cpp:461).
//             Leaving it to the kernel sweep frees the connection WITHOUT
//             removing that entry, and the guard in P2PeerConDmx::Listen
//             compares by POINTER -- so the next Dmx listener the allocator
//             places at the same address throws "Duplicate listen attempted
//             on single connection" on a hub that did nothing wrong.
//             Measured: 3 create/listen/close rounds were enough
//           - WAS UP, NOW DOWN (m_mapPeers[p] == false) is NOT signalled.
//             That is the redialling state -- MarkPeer(false) comes only from
//             On_ConClose, which is the very handler that Restart()s an
//             auto-restart dial -- and the exclusion is the original measured
//             restriction, kept.  Such a peer is also not the leak: it
//             connected once, so its con went through the kernel's normal
//             close path
//           - ...WITH ONE EXCEPTION, which is why m_setUnretired exists.  A
//             Disconnect whose budget expired leaves a peer looking exactly
//             like the redialling one above -- armed record erased, marked
//             down -- and its connection is still on this hub.  Reading the
//             maps cannot tell the two apart, so Disconnect writes the
//             difference down and this reads it back
//       : Signalling a never-connected con is safe for the reason CloseCon
//         documents and relies on already: P2PeerCon.cpp:97 leaves m_hCPort
//         NULL at construction, but PostP2Pmsg stamps the pump's IOCP onto it
//         on the way in (P2Pwin32.cpp:2587-2589) and nothing ever clears it,
//         so any con PostP2PeerCon returned TRUE for can be signalled.  This
//         is the same operation Link's unwind has performed since ABI 2
//       : Per-peer try/catch, not one around the loop.  A con that cannot be
//         signalled throws (P2PeerCon.cpp:968-971), and with one handler
//         around everything that would abandon the drain for every peer after
//         it -- turning one unretired connection into all of them
//       : DESTROY, not CLOSE: a facade dial sets m_uAutoRestart, so CLOSE
//         would just start the redial chain again (P2PeerTarget.cpp:2591)
//       : ConExists is the drain probe -- kernel truth (is the con still on
//         the hub's list?), not our own event bookkeeping, so a missed
//         OnPeerDown cannot stall the close
//       : Best-effort by design.  A hub whose pump is already wedged still
//         gets closed the old way -- no worse than before, and Close() must
//         never hang
//
BOOL
FacadeHub::DrainCons ( )
{
    // Snapshot all three classes; the maps are written on the pump threads.
    std::set<std::wstring> aSet;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );

      for ( std::map<std::wstring,bool>::const_iterator it = m_mapPeers.begin()
          ; it != m_mapPeers.end(); ++it )
        if ( it->second )
          aSet.insert ( it->first );

      for ( std::map<std::wstring,ArmedCon>::const_iterator it = m_mapArmed.begin()
          ; it != m_mapArmed.end(); ++it )
        if ( m_mapPeers.find ( it->first ) == m_mapPeers.end() )
          aSet.insert ( it->first );          // armed, never connected

      // Retired by Disconnect, but not observed to leave.  A set rather than a
      // fourth reading of the two maps because this class is exactly the one
      // they cannot express: the bookkeeping now says "was up, now down",
      // which is the redialling state, and the connection is neither.
      aSet.insert ( m_setUnretired.begin(), m_setUnretired.end() );
    }
    std::vector<std::wstring> aPeers ( aSet.begin(), aSet.end() );
    if ( aPeers.empty() )
      return TRUE;

    // Per peer, so one connection that refuses to be signalled cannot abandon
    // the drain for the rest.
    for ( size_t i = 0; i < aPeers.size(); ++i )
    {
      try { ConSignal ( aPeers[i].c_str(), P2PsigCon_DESTROY ); }
      catch ( ... ) { }
    }

    try
    {
      for ( int nSlice = 0; nSlice < kDrainSlices; ++nSlice )
      {
        BOOL bAnyLeft = FALSE;
        for ( size_t i = 0; i < aPeers.size() && !bAnyLeft; ++i )
          bAnyLeft = ConExists ( aPeers[i].c_str() );
        if ( !bAnyLeft )
          return TRUE;
        WaitSlice ( kDrainSliceMillisecs );
      }
    }
    catch ( ... )
    {
      // Hub context already gone (pump died, or a teardown race): the
      // kernel's own sweep is the remaining path.
    }
    return FALSE;
}

//
//  Stop the pump, having first got every client thread back out
//  NOTES: The Ping waiters are the reason this is not just CloseHub().  A
//         waiter is a CLIENT thread parked inside FacadeHub::Ping on an event
//         this object owns, and Close() ends in `delete this` -- so releasing
//         the waiters is only half the job.  The close must also KNOW they
//         have left, or the free races a thread that is about to touch
//         m_mapPings and m_oCSectPeers
//       : Three steps, and the order is the whole of the correctness.  Mark
//         first (under the lock, so a Ping either registered before the mark
//         or refuses after it and registers nothing), signal second, then wait
//         for the map to empty.  A waiter erases its own row while holding
//         m_oCSectPeers and touches nothing of this object afterwards, so an
//         empty map observed under that same lock means every waiter is out
//       : Bounded, like every other wait in this file.  A budget that runs out
//         leaves exactly the behaviour this had before -- no worse, and no
//         hang
//       : A CALLER-PUMPED hub takes the second branch below and never touches
//         a thread handle, because it has none.  The kernel's own teardown for
//         that shape is NOT CloseHub(): both of CloseHub's arms are guarded by
//         `nHubID != GetCurrentThreadId()` (P2PeerHub.cpp:268-292), so called
//         from the thread the hub runs on -- the only thread allowed to close
//         one of these -- it does nothing at all, comment about "synchronous
//         closure ... exists in this context" notwithstanding.  What ProcHub
//         does after RunHub returns is the model instead: zero the hub id, then
//         CloseP2PmsgHub(), which works on the CALLING thread's hub by
//         construction (P2Pwin32.cpp:2265-2285)
//
void
FacadeHub::CloseInternal ( )
{
    // Before the m_hThread guard: a hub that never started can still have had
    // a Ping called on it, and this flag is what makes that call refuse.
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      m_bClosing = true;

      // Wake anything blocked in Ping. The pump is about to stop, so no answer
      // can arrive; a waiter released now finds bAnswered false and reports
      // P2PF_E_TIMEOUT instead of sitting out its whole budget.
      for ( std::map<unsigned int,PingRow>::iterator it = m_mapPings.begin()
          ; it != m_mapPings.end(); ++it )
        if ( it->second.hEvent )
          ::SetEvent ( it->second.hEvent );
    }

    for ( int nSlice = 0; nSlice < kPingDrainSlices; ++nSlice )
    {
      {
        CSingleLock oLock ( &m_oCSectPeers, TRUE );
        if ( m_mapPings.empty() )
          break;
      }
      WaitSlice ( kDrainSliceMillisecs );
    }

    // The caller-pumped shape: no thread to stop, no handle to join.
    if ( m_bCallerPumped )
    {
      if ( !GetHubID() )
        return;                       // already torn down (dtor after Close)

      // From any other thread there is no honest teardown available: every
      // wait below needs the pump to run and only its owner can run it, and
      // CloseP2PmsgHub would be asking about the CALLER's hub, not this one.
      // Best effort, then, exactly as for a hub whose pump has wedged -- the
      // connections are signalled and the kernel's own CleanupP2Pmsg sweep is
      // what remains.  Close() refuses this outright; only the network's
      // release path, tidying up after a client that did not, can reach it.
      if ( !OwnsPump() )
      {
        DrainCons();
        return;
      }

      DrainCons();
      m_nHubID = 0;                   // ProcHub's StoreHubID(...,0), by hand
      try { CloseP2PmsgHub(); }
      catch ( ... ) { }
      return;
    }

    if ( !m_hThread )
      return;

    HANDLE hThread = m_hThread;
    m_hThread = 0;

    DrainCons();
    CloseHub();
    ::WaitForSingleObject ( hThread, 5000 );
    ::CloseHandle ( hThread );
}

HRESULT
FacadeHub::Close ( )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    // A caller-pumped hub can only be closed by the thread that runs it: the
    // teardown drains connections, and draining them means PUMPING them.  This
    // is checked BEFORE the once-only latch, so a refusal leaves the hub
    // usable rather than marking it closed and then declining to close it.
    if ( m_bCallerPumped && !OwnsPump() )
      return p2pf::P2PF_E_PUMP_OWNER;

    // Once only: a second Close() would double-delete.
    if ( ::InterlockedExchange ( &m_bClosed, 1 ) )
      return p2pf::P2PF_E_CLOSED;

    if ( m_pOwner )
      m_pOwner->RemoveHub ( this );

    CloseInternal();
    delete this;
    return S_OK;
}

///////////////////////////////////////////////////////////////////////
//  Connection arming

//
//  Hand one freshly manufactured connection to the kernel
//  NOTES: Without ConState_BCasts|ConState_UCasts the hub's broadcast and
//         unicast relays silently skip the connection
//       : PostP2PeerCon reports rejection (a connection with this peer
//         address is already registered on this hub) by RETURNING FALSE,
//         not by throwing
//       : A REJECTED CONNECTION IS ALREADY DESTROYED when PostP2PeerCon
//         returns, and must not be touched again here.  P2PeerHub.cpp:480
//         opens with `SafeP2PeerCon sppCon = pCon;` -- that takes a fresh
//         factory con from m_cRef 0 to 1, and every FALSE path returns
//         through its destructor, which Releases back to 0 and runs the
//         Destroy/delete path.  On success the con survives because
//         PostP2PmsgCon registered it first.  (Wrapping it again out here
//         is a use-after-free that aborts the process -- measured.)
//
HRESULT
FacadeHub::PostCon ( P2PeerCon *pCon )
{
    if ( !pCon )
      return p2pf::P2PF_E_CON_FACTORY;

    pCon->SetState ( ConState_BCasts | ConState_UCasts, 0 );

    BOOL bPosted = FALSE;
    try
    {
      bPosted = PostP2PeerCon ( pCon );
    }
    catch ( ... )
    {
      bPosted = FALSE;      // hub thread gone mid-teardown; con already freed
    }

    return bPosted ? S_OK : p2pf::P2PF_E_CON_DUPLICATE;
}

//
//  Manufacture one connection object for one endpoint
//  NOTES: THIS IS THE WHOLE DELTA one transport makes.  Everything before it
//         (AFX_MANAGE_STATE, the guards) and everything after it (PostCon) is
//         common; only the factory call and the shape of its arguments ever
//         varied, which is why eight typed verbs could collapse onto two
//       : Dials differ from listens by more than the factory name.  Tcp and
//         pipe dial through the RetryDial* wrappers so arming order never
//         matters; a dmx:// or serial:// endpoint the CALLER TYPED uses the
//         raw ClientFactory, because a missing in-process service or COM port
//         is a configuration fault rather than a timing one.  That policy is
//         per TRANSPORT and always was -- it is not a side effect of which
//         method name the caller typed, and collapsing the verbs did not
//         renegotiate it
//       : The ONE exception is a DERIVED dmx endpoint (rEp.bDerived), and it
//         is not really an exception to that rule so much as an application
//         of it.  A derived endpoint cannot be a configuration fault: nobody
//         configured it, and ResolveDial already refused unless a live
//         sibling hub had recorded a listener expecting us.  What is left is
//         a race -- that hub's listener arms later, on its own pump -- so the
//         derived dial gets the wrapper, bounded (see RetryDialDmx above).
//         Nothing a caller SPELLS OUT changes behaviour, in either direction
//       : No RetryDial232.  No endpoint is ever derived as serial, so the
//         class would have no caller; a typed serial:// dial keeps failing
//         fast, which is what its contract has always promised
//       : Every dmx connection is built by FacadeConDmx rather than by the
//         kernel's own factories, and for one reason only: those factories
//         never assign m_oP2Padomain, so the login accept filter was enforced
//         on three transports and silently skipped on the fourth.  See the
//         class above
//
P2PeerCon*
FacadeHub::MakeCon ( const FacadeEndpoint& rEp, const wchar_t *peer
                   , bool bListen )
{
    switch ( rEp.eKind )
    {
      case p2pfTcp:
        return bListen
             ? (P2PeerCon*)P2PeerConWsa::ServiceFactory ( peer, (short)rEp.uNum )
             : (P2PeerCon*)RetryDialWsa::Make ( peer, rEp.csHost, (short)rEp.uNum );

      case p2pfPipe:
        return bListen
             ? (P2PeerCon*)P2PeerConPipe::ServiceFactory ( peer, rEp.csName )
             : (P2PeerCon*)RetryDialPipe::Make ( peer, rEp.csName );

      case p2pfDmx:
        if ( bListen )
          return FacadeConDmx::Make ( peer, rEp.csName, true );
        return rEp.bDerived
             ? (P2PeerCon*)RetryDialDmx::Make ( peer, rEp.csName )
             : FacadeConDmx::Make ( peer, rEp.csName, false );

      case p2pfSerial:
        return bListen
             ? (P2PeerCon*)P2PeerCon232::ServiceFactory ( peer, (short)rEp.uNum )
             : (P2PeerCon*)P2PeerCon232::ClientFactory  ( peer, (short)rEp.uNum );
    }
    return 0;
}

//
//  Arm one connection and remember what it was armed with
//  NOTES: The record is written only on success, and only for a connection
//         THIS hub armed.  It is what an endpoint-less Connect on a sibling
//         hub resolves against, and it is deliberately the canonical spelling
//         rather than the caller's, so it round-trips back into Listen
//
HRESULT
FacadeHub::ArmRecorded ( const wchar_t *peer, const FacadeEndpoint& rEp
                       , bool bListen )
{
    // Record BEFORE posting, and roll back if the post is refused.
    //
    // The obvious order -- post, then record -- has a window the read side can
    // fall into.  PostP2PeerCon queues P2P_Startup and returns; the pump can
    // dequeue it, connect, log in and fire OnPeerUp while this thread is still
    // between the two statements.  A client that calls GetCon from its
    // OnPeerUp handler would then see a peer that is up with no endpoint and
    // no role -- which reads as "a peer this hub never armed", the one thing
    // an empty endpoint is supposed to mean.  Recording first makes the entry
    // visible from the instant the connection can be dispatched at all.
    // The rollback has to put back what was there, not just remove what this
    // call wrote.  The one arm that predictably fails is a DUPLICATE -- a
    // second Listen/Connect naming a peer this hub already armed -- and the
    // first connection is still live and still routing when the second is
    // refused.  Erasing unconditionally would report that live peer as one this
    // hub never armed: no endpoint, no listen/dial role, and P2PF_E_UNRESOLVED
    // from GetEndpoint.  The failed call must leave the read side exactly as it
    // found it.
    bool     bHadEntry = false;
    ArmedCon oPrev = { std::wstring(), false };
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );

      std::map<std::wstring,ArmedCon>::iterator it = m_mapArmed.find ( peer );
      bHadEntry = ( it != m_mapArmed.end() );
      if ( bHadEntry )
        oPrev = it->second;

      ArmedCon& rArmed   = m_mapArmed[peer];
      rArmed.strEndpoint = (LPCWSTR)rEp.Format();
      rArmed.bListen     = bListen;
    }

    HRESULT hr = PostCon ( MakeCon ( rEp, peer, bListen ) );
    if ( FAILED(hr) )
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      if ( bHadEntry ) m_mapArmed[peer] = oPrev;  // the earlier arm still stands
      else             m_mapArmed.erase ( peer ); // nothing was armed: claim nothing
    }
    return hr;
}

///////////////////////////////////////////////////////////////////////
//  The arming verbs

//
//  Resolve an omitted endpoint
//  NOTES: TWO TIERS, in this order, which is the plan of record's own
//         numbering (Opus C4a then C3) and the general rule that what a
//         deployment STATED beats what this process can INFER:
//           1. the configured map on the network, if one was ever set;
//           2. the in-process convention -- a derived Dmx service on a listen,
//              a live sibling hub's own record on a dial.
//         Nothing was ever configured until a client calls SetEndpoint*, so
//         tier 1 is absent by default and this is the behaviour it always had
//       : Tier 1 is keyed by ADDRESS and therefore serves both roles from one
//         table.  A DIAL wants the endpoint of its peer; a LISTEN wants the
//         endpoint of THIS hub, which is the same fact asked about a different
//         address.  The stored form is the DIAL spelling, so a listen drops
//         the host from it -- exactly the transformation Link performs on the
//         endpoint it is handed, and for the same reason: a listener cannot
//         know which host a dialer will use to reach it
//       : Tier 2, LISTEN, never probes.  A listener must be armable before the
//         hub that will dial it exists at all -- that is what listening is --
//         so it arms the derived Dmx name and waits
//       : Tier 2, DIAL, does probe, and fails loudly if the probe comes up
//         empty.  "Sibling hub present in this process" is the only fact that
//         makes an address sufficient on its own; a tcp host, a pipe name and
//         a COM port are deployment facts that cannot be derived from
//         "Demo.Server" by any means -- which is precisely the hole tier 1
//         fills, and why it reads a table rather than inventing one
//       : Note what is NOT here: a fallback that dials the derived Dmx name
//         at a sibling which has not RECORDED a listener for us.  It would
//         always return S_OK and then wait on a rendezvous nobody is coming
//         to.  P2PF_E_UNRESOLVED at the caller beats a connection that quietly
//         never happens
//       : Only TIER 2 is stamped bDerived, and the distinction is the point.
//         bDerived means "this process worked the endpoint out from live
//         state, so it can only be EARLY"; a map entry is a string a human
//         wrote, so it can be WRONG, and it fails fast like any other endpoint
//         a caller supplied (see RetryDialDmx)
//
HRESULT
FacadeHub::ResolveEndpoint ( const wchar_t *peer, bool bListen
                           , FacadeEndpoint& rEp )
{
    // Derivation reads this hub's own address, so it cannot work on a hub
    // that has not got one (the kernel also supports an address negotiated at
    // login; the facade only ever takes it from the constructor).
    if ( m_csAddress.IsEmpty() )
      return p2pf::P2PF_E_UNRESOLVED;

    // Tier 1 -- the deployment map. A dial asks about its PEER, a listen about
    // ITSELF; one table, two questions, because both are "where does this
    // address live".
    if ( m_pOwner )
    {
      const wchar_t *lpszWhose = bListen ? (LPCWSTR)m_csAddress : peer;
      if ( SUCCEEDED ( m_pOwner->LookupEndpoint ( lpszWhose, rEp ) ) )
      {
        if ( bListen )
          rEp.csHost.Empty();       // the dial form, minus the host
        return S_OK;                // NOT bDerived: a human wrote this one
      }
    }

    // Tier 2 -- the in-process convention.
    if ( bListen )
    {
      rEp.eKind    = p2pfDmx;
      rEp.csName   = DeriveDmxService ( (LPCWSTR)m_csAddress, peer );
      rEp.bDerived = true;
      return S_OK;
    }

    if ( !m_pOwner )
      return p2pf::P2PF_E_UNRESOLVED;

    HRESULT hr = m_pOwner->ResolveDial ( peer, (LPCWSTR)m_csAddress, rEp );
    if ( SUCCEEDED(hr) )
      rEp.bDerived = true;      // stamped after the parse ResolveDial runs
    return hr;
}

//
//  Arm one connection from a parsed endpoint, with the address guards
//  NOTES: The guards run BEFORE anything is manufactured, so a rejected call
//         never burns the one connection slot the kernel allows per peer
//         address on a hub
//
HRESULT
FacadeHub::ArmResolved ( const wchar_t *peer, const FacadeEndpoint& rEp
                       , bool bListen )
{
    if ( !peer )              return E_POINTER;
    if ( !*peer )             return E_INVALIDARG;
    if ( IsSwappedPeerArgument ( peer ) )
      return E_INVALIDARG;

    FacadeRelation eRel = ClassifyPeer ( (LPCWSTR)m_csAddress, peer );
    if ( eRel == p2pfRelSelf )
      return E_INVALIDARG;

    HRESULT hr = ArmRecorded ( peer, rEp, bListen );
    if ( FAILED(hr) )
      return hr;

    if ( eRel != p2pfRelUnrelated )
      return S_OK;

    // Armed, and direct traffic will work: RouteP2PeerMsg matches the peer's
    // own address before it consults the tree (P2PeerHub.cpp:723-725). What
    // this edge can never be is a TRANSIT hop -- both onward rules require an
    // ancestor/descendant relation (:727-739) and the broadcast relay
    // forwards only to children (:1235-1237), so anything addressed past this
    // peer dies as undeliverable and a broadcast arriving here stops here.
    // None of that is reported by the kernel, so say it once, and hand the
    // caller a success code it can test for rather than an error it would
    // have to learn to ignore.
    {
      CString csWhat;
      csWhat.Format ( L"TargetFacade: '%s' and peer '%s' are neither ancestor "
                      L"nor descendant. The link is armed and carries direct "
                      L"traffic, but nothing can be ROUTED through it and a "
                      L"broadcast will not relay beyond it."
                    , (LPCWSTR)m_csAddress, peer );
      RaiseEvent ( p2pf::P2PF_EVT_UNRELATED_LINK, peer, csWhat );
    }
    return p2pf::P2PF_S_UNRELATED_LINK;
}

HRESULT
FacadeHub::ArmUnified ( const wchar_t *peer, const wchar_t *endpoint
                      , bool bListen )
{
    if ( !peer )  return E_POINTER;
    if ( !*peer ) return E_INVALIDARG;
    if ( IsSwappedPeerArgument ( peer ) )
      return E_INVALIDARG;

    FacadeEndpoint oEp;
    HRESULT hr = ( endpoint && *endpoint )
               ? ParseFacadeEndpoint ( endpoint, bListen, oEp )
               : ResolveEndpoint     ( peer, bListen, oEp );
    if ( FAILED(hr) )
      return hr;

    return ArmResolved ( peer, oEp, bListen );
}

HRESULT
FacadeHub::Listen ( const wchar_t *toPeer, const wchar_t *endpoint )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    return ArmUnified ( toPeer, endpoint, true );
}

HRESULT
FacadeHub::Connect ( const wchar_t *toPeer, const wchar_t *endpoint )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    return ArmUnified ( toPeer, endpoint, false );
}

CString
FacadeHub::ArmedListener ( const wchar_t *peer ) const
{
    if ( !peer || !*peer )
      return CString();

    CSingleLock oLock ( &m_oCSectPeers, TRUE );
    std::map<std::wstring,ArmedCon>::const_iterator it = m_mapArmed.find ( peer );
    if ( it == m_mapArmed.end() || !it->second.bListen )
      return CString();
    return CString ( it->second.strEndpoint.c_str() );
}

//
//  Retract one connection this hub armed moments ago
//  NOTES: Exists for ONE caller: FacadeNetwork::Link, unwinding the listener
//         it armed when the dialer side then failed.  Without it "Link arms
//         both sides" would be a promise the facade cannot keep -- a
//         half-armed edge is a state no public call could clear, since
//         FacadeHub offers only whole-hub Close()
//       : Safe here for a reason that does NOT generalise.  DrainCons signals
//         only peers that are logged in, because a connection with no
//         completion port cannot be signalled (P2PeerCon.cpp:97 leaves
//         m_hCPort NULL at construction).  A connection that PostP2PeerCon
//         has RETURNED TRUE for is past that: PostP2Pmsg stamps the pump's
//         IOCP onto it on the way in (P2Pwin32.cpp:2588-2589).  So this is
//         callable on a just-armed, not-yet-connected connection and DrainCons
//         still must not be
//       : DESTROY rather than CLOSE, for the same reason DrainCons uses it:
//         a facade dial sets m_uAutoRestart, and CLOSE would merely start the
//         redial chain again
//       : Bounded and best effort, and the BUDGET IS THE CALLER'S because the
//         two callers retract two different things -- a registration that has
//         never connected, and a live session.  See kUnwindSlices /
//         kRetireSlices, which is where that difference is written down
//
BOOL
FacadeHub::CloseCon ( const wchar_t *peer, int nSlices )
{
    if ( !peer || !*peer )
      return FALSE;

    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      m_mapArmed.erase ( peer );
    }

    try
    {
      ConSignal ( peer, P2PsigCon_DESTROY );

      for ( int nSlice = 0; nSlice < nSlices; ++nSlice )
      {
        if ( !ConExists ( peer ) )
          return TRUE;
        WaitSlice ( kDrainSliceMillisecs );
      }
    }
    catch ( ... )
    {
      // Hub context already gone: nothing is armed there either.
      return TRUE;
    }
    return FALSE;
}

///////////////////////////////////////////////////////////////////////
//  Sending

//
//  Build and post one message
//  NOTES: The kernel dispatches locally when strDest names this hub,
//         otherwise RouteP2PeerMsg forwards it over the connections
//         (multi-hop; undeliverables bounce back as P2Pmsg_Exception)
//
HRESULT
FacadeHub::PostMsg ( const wchar_t *dest, P2PmsgID strName
                   , const void *pvData, unsigned int uSize
                   , const MsgOpts *pOpts )
{
    if ( uSize > p2pf::MAX_PAYLOAD )
      return E_INVALIDARG;
    if ( !pvData )
    {
      if ( uSize ) return E_POINTER;
      pvData = kEmptyPayload;
    }

    P2PeerMsg32 *pMsg = new P2PeerMsg32 ( (P2PaddrSTR)m_csAddress, dest
                                        , strName, pvData, (P2Psize_t)uSize );

    // The named fields (ABI 8), attached BEFORE the post for the same reason
    // the ABI 7 properties are: after it, the object is not ours.
    if ( pOpts && pOpts->pFields && !pOpts->pFields->Fields().empty() )
    {
      try
      {
        P3PmsgItem *pItem = FieldsItem ( pMsg, true );
        if ( pItem )
        {
          const std::vector<FacadeMessage::Field>& aF = pOpts->pFields->Fields();
          CString csNames;
          for ( size_t i = 0; i < aF.size(); ++i )
          {
            // (const void*) IS LOAD-BEARING. P3PmsgData's constructors
            // overload on LPCSTR, LPCWSTR and const void*, so a char* or a
            // wchar_t* silently selects a STRING constructor whose second
            // parameter is a LENGTH IN CHARACTERS rather than a size in
            // bytes. Measured: 26 bytes of wide text handed over as
            // (LPCWSTR) came back as 52. The cast is what selects the blob.
            pItem->DeclareItem ( aF[i].strName.c_str()
                               , P3PmsgData ( aF[i].aValue.empty()
                                                ? (const void*)kEmptyPayload
                                                : (const void*)&aF[i].aValue[0]
                                            , (VBLsize)aF[i].aValue.size()
                                            , VBLockData_BLOB16 )
                               , TRUE );
            if ( i ) csNames += L"\t";
            csNames += aF[i].strName.c_str();
          }
          // The index, beside the fields rather than inside them: a name is
          // not a field, and a client asking for the field called "P2PF$Names"
          // must get P2PF_E_NO_FIELD rather than this.
          pMsg->r_item ( VBLockBSTR_ROOT )
              .DeclareItem ( kNamesItem
                           , P3PmsgData ( (const void*)(LPCWSTR)csNames
                                        , (VBLsize)( ( csNames.GetLength() + 1 )
                                                     * sizeof(wchar_t) )
                                        , VBLockData_BLOB16 )
                           , TRUE );
        }
      }
      catch ( ... )
      {
        // The tree refused a field. Posting the message without it would
        // deliver a RECORD WITH A HOLE IN IT, which is worse than not
        // delivering: the receiver cannot tell a dropped field from one the
        // sender never set. So this one really does cost the message.
        delete pMsg;
        return E_FAIL;
      }
    }

    // The ABI 7 properties, applied BEFORE the post -- PostP2PeerMsg takes
    // ownership and the object is not ours to touch afterwards.
    //
    // Each is applied only when it was actually asked for, so a SendEx with
    // P2PF_PRI_DEFAULT / tag 0 / no flags posts exactly the message a plain
    // Send posts. That is not tidiness: the kernel's own header says to leave
    // priority alone unless you mean it, and a facade that "helpfully" stamped
    // Normal on everything would be making that choice for every client that
    // never asked for it.
    if ( pOpts )
    {
      try
      {
        if ( pOpts->uPriority != p2pf::P2PF_PRI_DEFAULT )
          pMsg->SetPriority ( (UCHAR)pOpts->uPriority );

        if ( pOpts->uTag )
        {
          // DWORD_PTR-wide in the kernel's prefix; the ABI carries 32 bits
          // because that is what every automation tier can hold, and because
          // a tag is a correlation token rather than a pointer.
          DWORD_PTR dwTag = (DWORD_PTR)pOpts->uTag;
          pMsg->SetAddrTag ( &dwTag, sizeof(dwTag) );
        }

        if ( pOpts->uFlags & p2pf::P2PF_SEND_NO_BOUNCE )
          P2PeerMsg_SetCtrlOptions ( pMsg, kCtrlNoBounce, 0xFF );
      }
      catch ( ... )
      {
        // A property the kernel would not accept must not cost the message.
        // Post it as built and let the client see it arrive without the
        // decoration, rather than fail a send over an adjective.
      }
    }

    try
    {
      PostP2PeerMsg ( pMsg );       // hub takes ownership
    }
    catch ( ... )
    {
      return p2pf::P2PF_E_CLOSED;   // hub thread gone mid-teardown
    }
    return S_OK;
}

HRESULT
FacadeHub::Send ( const wchar_t *dest, const wchar_t *topic
                , const void *payload, unsigned int size )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !dest || !topic )      return E_POINTER;
    if ( !*topic )              return E_INVALIDARG;
    // Two reserved namespaces now: the kernel's, and the facade's own (ABI 6 --
    // a client that could post a "P2PF$Post" could run code on another hub's
    // pump thread through its OnPost handler).
    if ( IsReservedName(topic) ||
         IsPrivateName (topic)  )
      return p2pf::P2PF_E_RESERVED_TOPIC;

    return PostMsg ( dest, topic, payload, size );
}

//
//  Send, saying the three things Send could not                       (ABI 7)
//  NOTES: Every guard is Send's, because this IS Send plus a MsgOpts.  The
//         alternative -- a separate validation path -- is how two verbs that
//         are meant to be the same verb drift apart
//
HRESULT
FacadeHub::SendEx ( const wchar_t *dest, const wchar_t *topic
                  , const void *payload, unsigned int size
                  , unsigned int priority, unsigned int tag
                  , unsigned int flags )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !dest || !topic )      return E_POINTER;
    if ( !*topic )              return E_INVALIDARG;
    if ( IsReservedName(topic) ||
         IsPrivateName (topic)  )
      return p2pf::P2PF_E_RESERVED_TOPIC;

    MsgOpts oOpts = { priority, tag, flags };
    return PostMsg ( dest, topic, payload, size, &oOpts );
}

HRESULT
FacadeHub::SendText ( const wchar_t *dest, const wchar_t *topic, const wchar_t *text )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !text ) return E_POINTER;
    size_t uBytes = ( ::wcslen(text) + 1 ) * sizeof(wchar_t);
    if ( uBytes > p2pf::MAX_PAYLOAD ) return E_INVALIDARG;

    return Send ( dest, topic, text, (unsigned int)uBytes );
}

HRESULT
FacadeHub::Broadcast ( const wchar_t *topic, const void *payload, unsigned int size )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    return BroadcastInternal ( topic, payload, size, 0 );
}

HRESULT
FacadeHub::BroadcastEx ( const wchar_t *topic
                       , const void *payload, unsigned int size
                       , unsigned int priority, unsigned int tag
                       , unsigned int flags )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    MsgOpts oOpts = { priority, tag, flags };
    return BroadcastInternal ( topic, payload, size, &oOpts );
}

//
//  Frame one broadcast and post a copy to every peer that is up
//  NOTES: The MsgOpts go on EVERY copy, which is what makes a tag mean "this
//         one sweep" at all twelve receivers rather than at whichever copy the
//         client happened to think of as the original
//
HRESULT
FacadeHub::BroadcastInternal ( const wchar_t *topic
                             , const void *payload, unsigned int size
                             , const MsgOpts *pOpts )
{
    if ( !topic )                return E_POINTER;
    if ( !*topic )               return E_INVALIDARG;
    if ( IsReservedName(topic) ||
         IsPrivateName (topic)  ) return p2pf::P2PF_E_RESERVED_TOPIC;
    if ( !payload && size )      return E_POINTER;

    size_t uTopicChars = ::wcslen ( topic );
    size_t uFrame      = sizeof(BCastFrame) + uTopicChars * sizeof(wchar_t) + size;
    if ( uTopicChars > 0xFFFF || uFrame > p2pf::MAX_PAYLOAD )
      return E_INVALIDARG;

    CByteArray aFrame;
    aFrame.SetSize ( (INT_PTR)uFrame );
    BCastFrame oHdr;
    oHdr.uMagic      = kBCastMagic;
    oHdr.uTopicChars = (UINT16)uTopicChars;
    ::memcpy ( aFrame.GetData(), &oHdr, sizeof(oHdr) );
    ::memcpy ( aFrame.GetData() + sizeof(oHdr), topic
             , uTopicChars * sizeof(wchar_t) );
    if ( size )
      ::memcpy ( aFrame.GetData() + sizeof(oHdr) + uTopicChars * sizeof(wchar_t)
               , payload, size );

    // Snapshot the live peers, then post outside the lock: PostP2PeerMsg can
    // run arbitrarily long and must never be called holding m_oCSectPeers
    // (a con callback on a pump thread wants that same lock).
    CStringArray aPeers;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      for ( std::map<std::wstring,bool>::const_iterator it = m_mapPeers.begin()
          ; it != m_mapPeers.end(); ++it )
        if ( it->second )
          aPeers.Add ( it->first.c_str() );
    }

    if ( aPeers.GetSize() == 0 )
      return S_FALSE;               // nobody logged in -- nothing was sent

    HRESULT hrLast = S_OK;
    for ( INT_PTR i = 0; i < aPeers.GetSize(); ++i )
    {
      HRESULT hr = PostMsg ( aPeers[i], P2Pmsg_BCast
                           , aFrame.GetData(), (unsigned int)uFrame, pOpts );
      if ( FAILED(hr) ) hrLast = hr;
    }
    return hrLast;
}

///////////////////////////////////////////////////////////////////////
//  Properties

const wchar_t*
FacadeHub::Address ( ) const
{
    return (P2PaddrSTR)m_csAddress;
}

BOOL
FacadeHub::IsPeerUp ( const wchar_t *peer ) const
{
    if ( !peer ) return FALSE;

    CSingleLock oLock ( &m_oCSectPeers, TRUE );
    std::map<std::wstring,bool>::const_iterator it = m_mapPeers.find ( peer );
    return ( it != m_mapPeers.end() && it->second ) ? TRUE : FALSE;
}

///////////////////////////////////////////////////////////////////////
//  Read side

namespace {

// The relation bit for one classification. Exactly one is ever set, so a
// caller can switch on `flags & P2PF_REL_MASK`.
unsigned int
RelationFlag ( FacadeRelation eRel )
{
    switch ( eRel )
    {
      case p2pfRelSelf:       return p2pf::P2PF_REL_SELF;
      case p2pfRelDescendant: return p2pf::P2PF_REL_DESCENDANT;
      case p2pfRelAncestor:   return p2pf::P2PF_REL_ANCESTOR;
      case p2pfRelUnrelated:  return p2pf::P2PF_REL_UNRELATED;
      case p2pfRelPattern:    return p2pf::P2PF_REL_PATTERN;
    }
    return p2pf::P2PF_REL_PATTERN;
}

// The flag vocabulary Describe prints. Order is fixed so the format is
// diffable between two snapshots.
void
AppendFlagText ( CString& rcs, unsigned int uFlags )
{
    if ( uFlags & p2pf::P2PF_CON_LISTEN ) rcs += L"listen,";
    if ( uFlags & p2pf::P2PF_CON_DIAL   ) rcs += L"dial,";
    if ( uFlags & p2pf::P2PF_CON_UP     ) rcs += L"up,";

    switch ( uFlags & p2pf::P2PF_REL_MASK )
    {
      case p2pf::P2PF_REL_SELF:       rcs += L"self";       break;
      case p2pf::P2PF_REL_DESCENDANT: rcs += L"descendant"; break;
      case p2pf::P2PF_REL_ANCESTOR:   rcs += L"ancestor";   break;
      case p2pf::P2PF_REL_UNRELATED:  rcs += L"unrelated";  break;
      default:                        rcs += L"pattern";    break;
    }
}

} // namespace

//
//  Collect every peer this hub knows, in one pass under one lock
//  NOTES: TWO sources, and the difference between them is the information.
//         m_mapArmed is what this hub asked for; m_mapPeers is who actually
//         turned up.  A wildcard listener arms "*" and then learns real names
//         at login, so the second set is not a subset of the first -- a hub
//         can legitimately list more peers than it armed connections
//       : Both maps are ordered by the same comparator, so the merge walks
//         them in step and the result is deterministic
//       : The relation is recomputed rather than cached.  It is three string
//         compares, it is wanted only on this diagnostic path, and caching it
//         would mean an adopted name (learned AFTER the arm) carried the
//         relation of the pattern it matched, which is exactly backwards
//
void
FacadeHub::SnapshotCons ( std::vector<ConRow>& rOut ) const
{
    rOut.clear();

    CSingleLock oLock ( &m_oCSectPeers, TRUE );
    rOut.reserve ( m_mapArmed.size() + m_mapPeers.size() );

    std::map<std::wstring,ArmedCon>::const_iterator itA = m_mapArmed.begin();
    std::map<std::wstring,bool>::const_iterator     itP = m_mapPeers.begin();

    while ( itA != m_mapArmed.end() || itP != m_mapPeers.end() )
    {
      bool bHasArmed = ( itA != m_mapArmed.end() );
      bool bHasPeer  = ( itP != m_mapPeers.end() );
      bool bTakeArmed = bHasArmed && ( !bHasPeer || itA->first <= itP->first );
      bool bTakePeer  = bHasPeer  && ( !bHasArmed || itP->first <= itA->first );

      ConRow oRow;
      oRow.strPeer  = bTakeArmed ? itA->first : itP->first;
      oRow.uFlags   = 0;

      if ( bTakeArmed )
      {
        oRow.strEndpoint = itA->second.strEndpoint;
        oRow.uFlags     |= itA->second.bListen ? p2pf::P2PF_CON_LISTEN
                                               : p2pf::P2PF_CON_DIAL;
        ++itA;
      }
      if ( bTakePeer )
      {
        if ( itP->second )
          oRow.uFlags |= p2pf::P2PF_CON_UP;
        ++itP;
      }

      oRow.uFlags |= RelationFlag ( ClassifyPeer ( (LPCWSTR)m_csAddress
                                                 , oRow.strPeer.c_str() ) );
      rOut.push_back ( oRow );
    }
}

HRESULT
FacadeHub::GetConCount ( unsigned int *outCount ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    if ( !outCount ) return E_POINTER;

    std::vector<ConRow> aRows;
    SnapshotCons ( aRows );
    *outCount = (unsigned int)aRows.size();
    return S_OK;
}

HRESULT
FacadeHub::GetCon ( unsigned int index
                  , wchar_t *peerBuf,     unsigned int *peerCch
                  , wchar_t *endpointBuf, unsigned int *endpointCch
                  , unsigned int *outFlags ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    std::vector<ConRow> aRows;
    SnapshotCons ( aRows );
    if ( index >= aRows.size() )
      return E_INVALIDARG;

    const ConRow& rRow = aRows[index];
    if ( outFlags )
      *outFlags = rRow.uFlags;

    // Both sizes are reported even when the first field does not fit, so one
    // failed call sizes BOTH buffers.
    HRESULT hrPeer = peerCch
                   ? CopyOut ( rRow.strPeer.c_str(), peerBuf, peerCch )
                   : S_OK;
    HRESULT hrEp   = endpointCch
                   ? CopyOut ( rRow.strEndpoint.c_str(), endpointBuf, endpointCch )
                   : S_OK;
    return FAILED(hrPeer) ? hrPeer : hrEp;
}

HRESULT
FacadeHub::GetEndpoint ( const wchar_t *peer, wchar_t *buf, unsigned int *cch ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    if ( !peer || !cch ) return E_POINTER;

    CString csEndpoint;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );

      std::map<std::wstring,ArmedCon>::const_iterator itA = m_mapArmed.find ( peer );
      if ( itA != m_mapArmed.end() )
        csEndpoint = itA->second.strEndpoint.c_str();
      else if ( m_mapPeers.find ( peer ) == m_mapPeers.end() )
        return p2pf::P2PF_E_UNRESOLVED;   // never armed, never seen
      // else: known but not armed by us -- empty, which is the true answer
    }
    return CopyOut ( csEndpoint, buf, cch );
}

HRESULT
FacadeHub::Describe ( wchar_t *buf, unsigned int *cch ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    if ( !cch ) return E_POINTER;

    std::vector<ConRow> aRows;
    SnapshotCons ( aRows );

    CString csOut;
    csOut.Format ( L"address=%s\n", (LPCWSTR)m_csAddress );
    for ( size_t i = 0; i < aRows.size(); ++i )
    {
      csOut += L"con=";
      csOut += aRows[i].strPeer.c_str();
      csOut += L"\t";
      csOut += aRows[i].strEndpoint.c_str();
      csOut += L"\t";
      AppendFlagText ( csOut, aRows[i].uFlags );
      csOut += L"\n";
    }
    return CopyOut ( csOut, buf, cch );
}

///////////////////////////////////////////////////////////////////////
//  Receive path

//
//  The extended sink, or NULL
//  NOTES: Written on a client thread by SetExtEvents and read on every pump
//         callback, so it is copied out under the lock and CALLED outside it.
//         Calling a client handler with m_oCSectPeers held would let any
//         handler that touches the read side deadlock its own hub
//
p2pf::IP2PHubEvents2*
FacadeHub::ExtEvents ( ) const
{
    CSingleLock oLock ( &m_oCSectPeers, TRUE );
    return m_pExtEvents;
}

//
//  Publish / retract the message GetMsgInfo answers about              (ABI 7)
//  NOTES: ORDER IS THE CORRECTNESS ARGUMENT, both ways round.  Going up, the
//         pointer is written before the thread id that makes it readable;
//         coming down, the thread id is retracted before the pointer is
//         cleared.  A thread that is not the one delivering therefore never
//         observes a published id with a pointer that is not yet -- or no
//         longer -- valid; it observes an id that is not its own and stops
//       : InterlockedExchange for the gate, plain stores for the rest: the
//         interlocked write is the release, and the only reader that goes past
//         the gate is the thread that did the writing
//
FacadeHub::CurMsgScope::CurMsgScope ( FacadeHub& rHub, P2PeerMsg *pMsg
                                    , bool bBCast )
        : m_rHub       ( rHub )
        , m_pPrevMsg   ( rHub.m_pCurMsg )
        , m_bPrevBCast ( rHub.m_bCurMsgBCast )
        , m_lPrevThread( rHub.m_lCurMsgThread )
{
    m_rHub.m_pCurMsg      = pMsg;
    m_rHub.m_bCurMsgBCast = bBCast;
    ::InterlockedExchange ( &m_rHub.m_lCurMsgThread,
                            (LONG)::GetCurrentThreadId() );
}

FacadeHub::CurMsgScope::~CurMsgScope ( )
{
    ::InterlockedExchange ( &m_rHub.m_lCurMsgThread, m_lPrevThread );
    m_rHub.m_pCurMsg      = m_pPrevMsg;
    m_rHub.m_bCurMsgBCast = m_bPrevBCast;
}

//
//  Surface one message to the client sink
//  NOTES: Runs on a pump thread.  A client handler that throws must not
//         unwind into the kernel's pump loop
//       : THE SUBSTITUTION RULE (ABI 6).  An extended sink replaces the plain
//         one for this callback rather than being called after it.  Delivering
//         to both would double every message for the one client shape that is
//         hardest to get right -- an object implementing both interfaces,
//         which is exactly what IP2PHubEvents2 deriving from IP2PHubEvents
//         encourages
//       : S_OK == handled (what OnMessage always meant, and the only answer a
//         plain sink can give); S_FALSE == declined, and the caller decides
//         whether that can be honoured.  A handler that throws counts as
//         handled: the message has been seen, and turning a client fault into
//         "unhandled message" would report it at the kernel as a routing
//         problem, which is the wrong diagnosis
//
HRESULT
FacadeHub::Deliver ( P2PeerMsg *pMsg, const wchar_t *lpszTopic
                   , const void *pvPayload, unsigned int uSize, bool bBCast )
{
    LPCWSTR lpszSource = L"";
    try { if ( pMsg && pMsg->GetSource() ) lpszSource = pMsg->GetSource(); }
    catch ( ... ) { }

    // Publish the message for GetMsgInfo, for the length of the callback and
    // not one instruction longer (ABI 7).  The scope object restores whatever
    // was published before rather than clearing to nothing: a handler that
    // sends and gets an answer cannot re-enter this, but a kernel that grows a
    // second pump could, and a saved/restored pair is correct either way for
    // the cost of two words on the stack.
    CurMsgScope oScope ( *this, pMsg, bBCast );

    p2pf::IP2PHubEvents2 *pExt = ExtEvents();
    if ( pExt )
    {
      try
      {
        return pExt->OnMessageEx ( lpszSource, lpszTopic ? lpszTopic : L""
                                 , pvPayload, uSize, bBCast );
      }
      catch ( ... ) { return S_OK; }
    }

    if ( m_pEvents )
    {
      try
      {
        m_pEvents->OnMessage ( lpszSource, lpszTopic ? lpszTopic : L""
                             , pvPayload, uSize, bBCast );
      }
      catch ( ... )
      {
        // A client handler threw; swallow it rather than unwind into the pump.
      }
    }
    return S_OK;
}

//
//  The message being delivered on THIS thread, or NULL
//  NOTES: The thread gate is the whole safety argument.  m_lCurMsgThread is
//         published after the pointer and retracted before it, so a thread
//         that is not the one inside the callback never reaches the pointer
//         at all -- it reads an id that is not its own and stops here
//       : One function, so GetMsgInfo and the three field readers cannot
//         drift apart on the one rule they all depend on
//
P2PeerMsg*
FacadeHub::CurMsg ( ) const
{
    if ( (DWORD)::InterlockedCompareExchange (
                    const_cast<volatile LONG*>(&m_lCurMsgThread), 0, 0 )
         != ::GetCurrentThreadId() )
      return 0;
    return m_pCurMsg;
}

//
//  The facade's field item on one message, or NULL              (ABI 8)
//  NOTES: A child of the message ROOT, beside the kernel's own Net/Msg/Wrp/Evt
//         -- see kFieldsItem for why there and not in the payload
//       : Every call is wrapped by the caller; the tree throws rather than
//         returning errors, and a throw out of here would unwind into the pump
//
P3PmsgItem*
FacadeHub::FieldsItem ( P2PeerMsg *pMsg, bool bCreate )
{
    if ( !pMsg )
      return 0;

    P3PmsgItem& oRoot = pMsg->r_item ( VBLockBSTR_ROOT );
    if ( !oRoot.Exists ( kFieldsItem ) )
    {
      if ( !bCreate )
        return 0;
      return &oRoot.DeclareItem ( kFieldsItem, P3PmsgData() );
    }
    return &oRoot.SelectItem ( kFieldsItem );
}

//
//  The field names, in insertion order                          (ABI 8)
//  NOTES: Read from the facade's own index item rather than walked, because
//         the P3PmsgField that actually compiles has no child count and no
//         cursor -- see kFieldsItem.  Tab-separated because a field name may
//         contain anything else a client likes, and a tab is the one character
//         nothing sensible puts in an identifier
//
BOOL
FacadeHub::FieldNames ( P2PeerMsg *pMsg, CStringArray& rOut )
{
    rOut.RemoveAll();
    if ( !pMsg )
      return FALSE;

    CString csAll;
    try
    {
      P3PmsgItem& oRoot = pMsg->r_item ( VBLockBSTR_ROOT );
      if ( !oRoot.Exists ( kNamesItem ) )
        return FALSE;
      P3PmsgItem& oNames = oRoot.SelectItem ( kNamesItem );
      const void  *pv = oNames.c_vBlob();
      unsigned int n  = (unsigned int)oNames.r_data().c_size();
      if ( !pv || n < sizeof(wchar_t) )
        return FALSE;
      // Stored with its terminator, so the last wchar_t is one.
      // Scan to the terminator rather than trusting the reported size to be
      // the exact byte count written. It is, now that the blob constructor is
      // being selected correctly -- but a names index that runs off its own
      // end invents field names, and stopping at the NUL costs nothing.
      const wchar_t *pw   = (const wchar_t*)pv;
      unsigned int   uMax = n / sizeof(wchar_t);
      unsigned int   uLen = 0;
      while ( uLen < uMax && pw[uLen] ) ++uLen;
      csAll = CString ( pw, (int)uLen );
    }
    catch ( ... ) { return FALSE; }

    int nPos = 0;
    for ( ;; )
    {
      CString csOne = csAll.Tokenize ( L"\t", nPos );
      if ( csOne.IsEmpty() && nPos < 0 ) break;
      if ( !csOne.IsEmpty() ) rOut.Add ( csOne );
      if ( nPos < 0 ) break;
    }
    return TRUE;
}

//
//  What else is true of the message being delivered right now       (ABI 7)
//  NOTES: Answering P2PF_E_NO_MESSAGE rather than zeroes matters: "the sender
//         set no tag" and "you asked in the wrong place" are different
//         answers, and a client that mixes them up correlates against 0
//
HRESULT
FacadeHub::GetMsgInfo ( wchar_t *destBuf, unsigned int *destCch
                      , unsigned int *outPriority
                      , unsigned int *outTag
                      , unsigned int *outFlags ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    P2PeerMsg *pMsg = CurMsg();
    if ( !pMsg )
      return p2pf::P2PF_E_NO_MESSAGE;

    if ( outPriority )
    {
      *outPriority = p2pf::P2PF_PRI_NORMAL;
      try { *outPriority = (unsigned int)pMsg->Priority(); } catch ( ... ) { }
    }

    if ( outTag )
    {
      *outTag = 0;
      try { *outTag = (unsigned int)( pMsg->AddrTag() & 0xFFFFFFFFu ); }
      catch ( ... ) { }
    }

    if ( outFlags )
    {
      unsigned int uFlags = m_bCurMsgBCast ? p2pf::P2PF_MSG_BROADCAST : 0;
      // Reported the way a client reads it -- "will a decline be heard?" --
      // rather than the way it is stored, which is the suppression bit.
      if ( !IsNoBounce ( pMsg ) )      uFlags |= p2pf::P2PF_MSG_BOUNCES;
      try { if ( pMsg->IsWrapped()   ) uFlags |= p2pf::P2PF_MSG_WRAPPED;   }
      catch ( ... ) { }
      try { if ( pMsg->IsReflected() ) uFlags |= p2pf::P2PF_MSG_REFLECTED; }
      catch ( ... ) { }
      try { if ( FieldsItem ( pMsg, false ) ) uFlags |= p2pf::P2PF_MSG_FIELDS; }
      catch ( ... ) { }
      *outFlags = uFlags;
    }

    if ( destCch )
    {
      CString csDest;
      try { if ( pMsg->GetDestin() ) csDest = (P2PaddrSTR)pMsg->GetDestin(); }
      catch ( ... ) { }
      return CopyOut ( csDest, destBuf, destCch );
    }
    return S_OK;
}

///////////////////////////////////////////////////////////////////////
//  Fields                                                       (ABI 8)

//
//  Will this message fit one P2PeerMsg?
//  NOTES: Charges each field its NAME as well as its value, because both are
//         on the wire, plus a few bytes of node overhead apiece.  The estimate
//         is deliberately pessimistic: refusing a message that would have just
//         fitted costs a caller one error, and accepting one that does not fit
//         costs everyone the connection
//
HRESULT
FacadeHub::CheckTotalSize ( const FacadeMessage *pMsg )
{
    // Per-node overhead in the P3Pmsg tree: a type byte, a length and an
    // address, doubled for headroom rather than reverse-engineered exactly.
    const size_t kPerFieldOverhead = 32;

    size_t uTotal = pMsg->Payload().size();
    const std::vector<FacadeMessage::Field>& aF = pMsg->Fields();
    for ( size_t i = 0; i < aF.size(); ++i )
      uTotal += aF[i].aValue.size()
              + aF[i].strName.size() * sizeof(wchar_t)
              + kPerFieldOverhead;

    return ( uTotal > p2pf::MAX_PAYLOAD ) ? E_INVALIDARG : S_OK;
}

//
//  Send a message object
//  NOTES: Every guard is Send's, because this IS Send with a different way of
//         spelling its payload -- the same reason SendEx repeats them
//       : THE SIZE LIMIT IS ON THE TOTAL.  MAX_PAYLOAD bounds what one
//         P2PeerMsg may carry, and fields ride in the same message; a per-field
//         cap alone would let 64 legal fields build one illegal message, and
//         the kernel's answer to an oversize frame is to DROP THE CONNECTION.
//         So the sum is checked here, where both halves are known
//
HRESULT
FacadeHub::SendMsg ( const wchar_t *dest, const wchar_t *topic
                   , p2pf::IP2PMessage *msg
                   , unsigned int priority, unsigned int tag
                   , unsigned int flags )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !dest || !topic || !msg ) return E_POINTER;
    if ( !*topic )                 return E_INVALIDARG;
    if ( IsReservedName(topic) ||
         IsPrivateName (topic)  )
      return p2pf::P2PF_E_RESERVED_TOPIC;

    const FacadeMessage *pFm = static_cast<const FacadeMessage*>(msg);
    HRESULT hr = CheckTotalSize ( pFm );
    if ( FAILED(hr) ) return hr;

    MsgOpts oOpts = { priority, tag, flags, pFm };
    return PostMsg ( dest, topic
                   , pFm->Payload().empty() ? 0 : &pFm->Payload()[0]
                   , (unsigned int)pFm->Payload().size(), &oOpts );
}

HRESULT
FacadeHub::BroadcastMsg ( const wchar_t *topic, p2pf::IP2PMessage *msg
                        , unsigned int priority, unsigned int tag
                        , unsigned int flags )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !msg ) return E_POINTER;

    const FacadeMessage *pFm = static_cast<const FacadeMessage*>(msg);
    HRESULT hr = CheckTotalSize ( pFm );
    if ( FAILED(hr) ) return hr;

    MsgOpts oOpts = { priority, tag, flags, pFm };
    return BroadcastInternal ( topic
                             , pFm->Payload().empty() ? 0 : &pFm->Payload()[0]
                             , (unsigned int)pFm->Payload().size(), &oOpts );
}

HRESULT
FacadeHub::GetFieldCount ( unsigned int *outCount ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outCount ) return E_POINTER;
    *outCount = 0;

    P2PeerMsg *pMsg = CurMsg();
    if ( !pMsg )
      return p2pf::P2PF_E_NO_MESSAGE;

    CStringArray aNames;
    if ( FieldNames ( pMsg, aNames ) )
      *outCount = (unsigned int)aNames.GetSize();
    return S_OK;                        // no fields is a count, not an error
}

HRESULT
FacadeHub::GetFieldName ( unsigned int index
                        , wchar_t *buf, unsigned int *cch ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    P2PeerMsg *pMsg = CurMsg();
    if ( !pMsg )
      return p2pf::P2PF_E_NO_MESSAGE;

    CStringArray aNames;
    if ( !FieldNames ( pMsg, aNames ) || index >= (unsigned int)aNames.GetSize() )
      return E_INVALIDARG;

    return CopyOut ( aNames[(INT_PTR)index], buf, cch );
}

//
//  One field off the message being delivered
//  NOTES: P2PF_E_NO_FIELD and "present but empty" are DIFFERENT answers, and
//         keeping them apart is why this cannot reuse the string CopyOut:
//         presence is a signal in its own right, and a client that cannot see
//         the difference has to encode one in the value
//
HRESULT
FacadeHub::GetField ( const wchar_t *name, void *buf, unsigned int *size ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !name || !size ) return E_POINTER;

    P2PeerMsg *pMsg = CurMsg();
    if ( !pMsg )
      return p2pf::P2PF_E_NO_MESSAGE;

    const void  *pvValue = 0;
    unsigned int uSize   = 0;
    try
    {
      P3PmsgItem *pItem = FieldsItem ( pMsg, false );
      if ( !pItem || !pItem->Exists ( name ) )
        return p2pf::P2PF_E_NO_FIELD;

      P3PmsgItem& oField = pItem->SelectItem ( name );
      pvValue = oField.c_vBlob();
      uSize   = (unsigned int)oField.r_data().c_size();
    }
    catch ( ... ) { return p2pf::P2PF_E_NO_FIELD; }

    return CopyOutBytes ( pvValue, uSize, buf, size );
}

//
//  Say that a message was declined, on a path that cannot simply pass it on
//  NOTES: A topic-named unicast honours "not handled" by returning msgCONTINUE
//         -- the message travels on down the handler chain and ends at the
//         kernel's own NotHandled, which posts a P2Pmsg_Exception back to the
//         sender.  That is what the answer MEANS: the sender is told
//       : A broadcast cannot do that.  The base P2PeerHub handler is the relay
//         that forwards it to this hub's children, so it must run and it
//         always answers msgHANDLED.  Ignoring the client's answer there was
//         the honest description of the old behaviour and the wrong behaviour:
//         the same S_FALSE meant "the sender learns" on one path and nothing
//         at all on the other
//       : So call the kernel's own reporter directly, after the relay.  Same
//         function, same exception, same event at the sender -- the client's
//         answer now means one thing everywhere, and the relay is untouched
//       : Only ever reached when an EXTENDED sink returned S_FALSE.  A plain
//         sink cannot answer, and an extended one that does not decline never
//         gets here, so no existing client's traffic changes shape
//
void
FacadeHub::ReportDeclined ( P2PeerMsg *pMsg )
{
    if ( !pMsg )
      return;

    // The sender said not to (ABI 7, P2PF_SEND_NO_BOUNCE).  This is the ONE
    // place the suppression can be honoured, and honouring it here is what
    // makes it worth having: it stops the exception being GENERATED, so
    // nothing travels back and no hub spends anything on it.
    //
    // It does not, and cannot, cover the other bounce -- an address that could
    // not be routed to at all.  That one is posted by the kernel's own
    // RouteP2PeerMsg before any facade code is reached.  Suppressing a
    // decline and suppressing a wrong address are different promises and only
    // the first is one this layer can keep, so only the first is made.
    if ( IsNoBounce ( pMsg ) )
      return;

    // NotHandled builds a P2Pevent and posts an exception; a throw out of it
    // would unwind into the pump loop.
    try { NotHandled ( pMsg ); } catch ( ... ) { }
}

//
//  Surface one bounced message
//  NOTES: THE BOUNCE HAS ALWAYS COME BACK ON A DIFFERENT MESSAGE than the one
//         this facade was watching for.  P2PF_EVT_ROUTING_ERROR was raised
//         from On_P2PeerError, which the kernel dispatches for P2Pmsg_Error --
//         and nothing in TargetCore posts one: the only factory that did is
//         commented out (P2PeerMsg.cpp:660-676).  What the kernel actually
//         sends back, from RouteP2PeerMsg for an undeliverable message and
//         from NotHandled for one nobody handled, is a P2Pmsg_EXCEPTION.  It
//         arrived, was caught by P2PeerTarget's own CATCH entry, printed by
//         the event system and never mentioned to the client -- so the code
//         whose comment says "the kernel bounced a message back" never fired
//         for a bounce
//       : Read here on the way past and then handed on, so the kernel's own
//         handling (which owns the event's life cycle) is untouched.  The copy
//         extracted here is cancelled WITHOUT notification, because the base
//         handler still displays its own
//       : The peer is the SOURCE of the exception, which ExceptionFactory
//         swapped -- that is the hub that could not deliver or would not
//         handle the message, i.e. exactly the peer the client wants named
//
void
FacadeHub::ReportBounce ( P2PeerMsg *pMsg )
{
    // NOT a bounce of a bounce.
    //
    // The kernel's own CATCH entries match on the name of the message INSIDE
    // an exception (P2Pmsg_Undeliv, P2Pmsg_BCast, P2Pmsg_Exception), so an
    // exception wrapping an ordinary client topic matches none of them and is
    // itself bounced one hop further -- where the doubly-wrapped form DOES
    // match (On_MsgCatchCatch) and stops. That extra hop is the kernel's, and
    // predates this: nothing here changes it.
    //
    // What must not happen is REPORTING it. That hop travels back towards the
    // hub that declined or could not deliver -- which sent nothing and has
    // nothing to be told. A bounce is reported to the sender of the message
    // that bounced, and a bounce is not a message anyone sent.
    try
    {
      if ( pMsg->IsWrapped() && (*pMsg)[1].Map_MatchName ( P2Pmsg_Exception ) )
        return;
    }
    catch ( ... ) { }

    CString csPeer, csWhy;
    try { if ( pMsg->GetSource() ) csPeer = (P2PaddrSTR)pMsg->GetSource(); }
    catch ( ... ) { }

    try
    {
      P2Pevent *pEVT = pMsg->ExtractP2Pevent();
      if ( pEVT )
      {
        csWhy = pEVT->GetMessage();
        pEVT->Cancel ( false );        // our copy; the base handler shows its own
      }
    }
    catch ( ... ) { }

    CString csWhat;
    if ( csWhy.IsEmpty() )
      csWhat.Format ( L"TargetFacade: a message sent to '%s' came back "
                      L"undelivered or unhandled."
                    , (LPCWSTR)csPeer );
    else
      csWhat.Format ( L"TargetFacade: a message sent to '%s' came back: %s"
                    , (LPCWSTR)csPeer, (LPCWSTR)csWhy );

    RaiseEvent ( p2pf::P2PF_EVT_ROUTING_ERROR, csPeer, csWhat );
}

//
//  Report one condition
//  NOTES: The prose sentence was the only push report this facade had, and the
//         header has always said not to branch on its wording.  An extended
//         sink gets the same sentence WITH a code and the peer it is about;
//         a plain sink gets what it always got.  Same substitution rule as
//         Deliver, and for the same reason
//
void
FacadeHub::RaiseEvent ( unsigned int uCode, const wchar_t *lpszPeer
                      , const wchar_t *lpszWhat )
{
    p2pf::IP2PHubEvents2 *pExt = ExtEvents();
    if ( pExt )
    {
      try { pExt->OnEvent ( uCode, lpszPeer ? lpszPeer : L"", lpszWhat ); }
      catch ( ... ) { }
      return;
    }
    if ( m_pEvents )
    {
      try { m_pEvents->OnError ( lpszWhat ); } catch ( ... ) { }
    }
}

//
//  One "P2PF$" message, on the pump thread
//  NOTES: Ping and Pong are the only two accepted from ANOTHER hub -- they are
//         a round trip between two hubs, so they have to be.  Everything else
//         is this hub talking to its own pump on behalf of a client thread,
//         and is refused unless the source is this hub's own address.  Without
//         that check a remote peer could arm timers here, or drive this
//         process's OnPost handler with a pointer it chose
//       : Nothing here ever reaches the client sink as a message.  These are
//         the facade's own plumbing, and a client that saw them would have to
//         learn to ignore them
//
BOOL
FacadeHub::HandlePrivate ( P2PeerMsg *pMsg, const wchar_t *lpszTopic )
{
    const char *pvData = 0;
    size_t      uSize  = 0;
    try { pvData = pMsg->Data(); uSize = pMsg->DataSize(); }
    catch ( ... ) { pvData = 0; uSize = 0; }

    LPCWSTR lpszSource = L"";
    try { if ( pMsg->GetSource() ) lpszSource = pMsg->GetSource(); }
    catch ( ... ) { }

    // --- the round trip, from anywhere ------------------------------------
    if ( ::wcscmp ( lpszTopic, kTopicPing ) == 0 )
    {
      // Echo the body back verbatim: the id in it is the requester's, and
      // this side never interprets it.
      if ( *lpszSource )
        PostMsg ( lpszSource, kTopicPong, pvData, (unsigned int)uSize );
      return TRUE;
    }

    if ( ::wcscmp ( lpszTopic, kTopicPong ) == 0 )
    {
      if ( pvData && uSize >= sizeof(PrivPingBody) )
      {
        PrivPingBody oBody;
        ::memcpy ( &oBody, pvData, sizeof(oBody) );

        CSingleLock oLock ( &m_oCSectPeers, TRUE );
        std::map<unsigned int,PingRow>::iterator it = m_mapPings.find ( oBody.uPingId );
        if ( it != m_mapPings.end() && !it->second.bAnswered )
        {
          ULONGLONG uNow = ::GetTickCount64();
          it->second.uMillisecs = (unsigned int)( uNow - it->second.uSentTick );
          it->second.bAnswered  = true;
          if ( it->second.hEvent )
            ::SetEvent ( it->second.hEvent );
        }
      }
      return TRUE;
    }

    // --- this hub's own plumbing, from this hub only -----------------------
    if ( ::wcscmp ( lpszSource, (LPCWSTR)m_csAddress ) != 0 )
      return TRUE;                     // consumed, deliberately ignored

    if ( ::wcscmp ( lpszTopic, kTopicTimer ) == 0 &&
         pvData && uSize >= sizeof(PrivTimerBody) )
    {
      PrivTimerBody oBody;
      ::memcpy ( &oBody, pvData, sizeof(oBody) );

      // The deadline SetTimer stamped, not the delay it was given: the trip
      // that got this message here has already spent some of that delay.
      ULONGLONG uDeadline = 0;
      {
        CSingleLock oLock ( &m_oCSectPeers, TRUE );
        std::map<unsigned int,TimerRow>::const_iterator it
          = m_mapTimers.find ( oBody.uTimerId );
        if ( it == m_mapTimers.end() )
          return TRUE;                 // KillTimer got here first
        uDeadline = it->second.uDeadline;
      }

      // ON THE PUMP, which is the whole reason this took a message to get
      // here: SetPITimer resolves pump 0 = "the pump running this thread"
      // (P2PeerTarget.cpp -> SetP2PmsgTimer(0,this,...)), so arming it from
      // the client thread would have armed it on no pump at all.
      PITimerID nKernel = ArmPumpTimer ( uDeadline, oBody.uTimerId, false );

      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      std::map<unsigned int,TimerRow>::iterator it = m_mapTimers.find ( oBody.uTimerId );
      if ( it != m_mapTimers.end() )
      {
        if ( nKernel ) it->second.nKernelID = nKernel;
        else           m_mapTimers.erase ( it );   // never armed: forget it
      }
      else if ( nKernel )
      {
        // KillTimer got here first. Retract what was just armed.
        try { ::KillP2PmsgTimer ( nKernel ); } catch ( ... ) { }
      }
      return TRUE;
    }

    if ( ::wcscmp ( lpszTopic, kTopicKill ) == 0 &&
         pvData && uSize >= sizeof(PrivKillBody) )
    {
      PrivKillBody oBody;
      ::memcpy ( &oBody, pvData, sizeof(oBody) );

      PITimerID nKernel = 0;
      {
        CSingleLock oLock ( &m_oCSectPeers, TRUE );
        std::map<unsigned int,TimerRow>::iterator it = m_mapTimers.find ( oBody.uTimerId );
        if ( it != m_mapTimers.end() )
        {
          nKernel = it->second.nKernelID;
          m_mapTimers.erase ( it );
        }
      }
      if ( nKernel )
        try { ::KillP2PmsgTimer ( nKernel ); } catch ( ... ) { }
      return TRUE;
    }

    if ( ::wcscmp ( lpszTopic, kTopicPost ) == 0 &&
         pvData && uSize >= sizeof(PrivPostBody) )
    {
      PrivPostBody oBody;
      ::memcpy ( &oBody, pvData, sizeof(oBody) );

      p2pf::IP2PHubEvents2 *pExt = ExtEvents();
      if ( pExt )
      {
        try { pExt->OnPost ( (unsigned int)oBody.uKey
                           , (void*)(UINT_PTR)oBody.uContext ); }
        catch ( ... ) { }
      }
      return TRUE;
    }

    return TRUE;                       // unknown private topic: consumed
}

msgRESULT
FacadeHub::On_AnyTopic ( P2PeerMsg *pMsg )
{
    if ( !pMsg )
      return msgCONTINUE;

    if ( IsReservedName ( pMsg->c_name() ) )
    {
      // One kernel message the facade reads on the way past. Everything else
      // in that namespace belongs to the P2PeerHub defaults and is left alone.
      bool bBounce = false;
      try { bBounce = pMsg->Map_MatchName ( P2Pmsg_Exception ) ? true : false; }
      catch ( ... ) { }
      if ( bBounce )
        ReportBounce ( pMsg );

      return msgCONTINUE;   // kernel message -- let the P2PeerHub defaults run
    }

    LPCTNAM lpszTopic = pMsg->c_name();
    if ( IsPrivateName ( lpszTopic ) )
    {
      HandlePrivate ( pMsg, lpszTopic );
      return msgHANDLED;
    }

    size_t uSize = 0;
    const char *pvData = 0;
    try { pvData = pMsg->Data(); uSize = pMsg->DataSize(); }
    catch ( ... ) { pvData = 0; uSize = 0; }

    // This is the ONE path where an extended sink's "not handled" can be
    // honoured: nothing downstream of here belongs to the facade, so the
    // message can simply travel on to the P2PeerHub defaults -- which is
    // where a genuinely unknown topic gets reported as unknown.
    HRESULT hr = Deliver ( pMsg, lpszTopic, pvData, (unsigned int)uSize, false );

    // A decline travels on down the handler chain, which ends at the kernel's
    // NotHandled and tells the sender -- unless the sender asked not to be
    // told (ABI 7).  Then it stops here, which is the same suppression
    // ReportDeclined performs on the broadcast path, applied at the one point
    // this path can apply it.
    if ( hr == S_FALSE && !IsNoBounce ( pMsg ) )
      return msgCONTINUE;
    return msgHANDLED;
}

msgRESULT
FacadeHub::On_P2PeerBCast ( P2PeerMsg *pMsg )
{
    bool bDeclined = false;
    if ( pMsg )
    {
      const char *pvData = 0;
      size_t      uSize  = 0;
      try { pvData = pMsg->Data(); uSize = pMsg->DataSize(); }
      catch ( ... ) { pvData = 0; uSize = 0; }

      BCastFrame oHdr = { 0, 0 };
      if ( pvData && uSize >= sizeof(BCastFrame) )
        ::memcpy ( &oHdr, pvData, sizeof(oHdr) );

      size_t uTopicBytes = (size_t)oHdr.uTopicChars * sizeof(wchar_t);
      if ( pvData                                   &&
           oHdr.uMagic == kBCastMagic               &&
           uSize >= sizeof(BCastFrame) + uTopicBytes  )
      {
        // Framed by this facade: topic then payload.
        CStringW csTopic ( (const wchar_t*)( pvData + sizeof(BCastFrame) )
                         , (int)oHdr.uTopicChars );
        bDeclined = ( Deliver ( pMsg, csTopic
                              , pvData + sizeof(BCastFrame) + uTopicBytes
                              , (unsigned int)( uSize - sizeof(BCastFrame)
                                                      - uTopicBytes )
                              , true ) == S_FALSE );
      }
      else
      {
        // Unframed broadcast from a non-facade peer: hand over the raw body
        // under the kernel message name.
        bDeclined = ( Deliver ( pMsg, pMsg->c_name(), pvData
                              , (unsigned int)uSize, true ) == S_FALSE );
      }
    }

    // MUST delegate: the base handler is the relay that forwards a broadcast
    // on to this hub's child connections.  So the client's "not handled" is
    // honoured AFTER it, by reporting the message the way the kernel's own
    // handler chain would have -- see ReportDeclined.
    msgRESULT r = P2PeerHub::On_P2PeerBCast ( pMsg );
    if ( bDeclined )
      ReportDeclined ( pMsg );
    return r;
}

//
//  NOTES: Unreachable in this kernel, and kept anyway.  Nothing dispatches
//         On_P2PeerUCast -- P2PeerHub's map carries entries for P2Pmsg_BCast
//         and P2Pmsg_Error only, and no other code path calls the virtual --
//         so a "P2Pmsg*"-named unicast that this hub's wildcard passes on ends
//         at NotHandled without ever arriving here.  The override stays
//         because the vtable slot exists and a kernel that starts using it
//         must find the facade's semantics in it, not the base's
//
msgRESULT
FacadeHub::On_P2PeerUCast ( P2PeerMsg *pMsg )
{
    bool bDeclined = false;
    if ( pMsg )
    {
      const char *pvData = 0;
      size_t      uSize  = 0;
      try { pvData = pMsg->Data(); uSize = pMsg->DataSize(); }
      catch ( ... ) { pvData = 0; uSize = 0; }
      bDeclined = ( Deliver ( pMsg, pMsg->c_name(), pvData
                            , (unsigned int)uSize, false ) == S_FALSE );
    }

    msgRESULT r = P2PeerHub::On_P2PeerUCast ( pMsg );
    if ( bDeclined )
      ReportDeclined ( pMsg );
    return r;
}

msgRESULT
FacadeHub::On_P2PeerError ( P2PeerMsg *pMsg )
{
    LPCWSTR lpszWhat = L"P2Pmsg routing error";
    try { if ( pMsg && pMsg->GetVisualRTSummary() ) lpszWhat = pMsg->GetVisualRTSummary(); }
    catch ( ... ) { }

    // The peer this is about is the best guess available: the source of the
    // message that bounced. Empty rather than invented when there is none.
    LPCWSTR lpszPeer = L"";
    try { if ( pMsg && pMsg->GetSource() ) lpszPeer = pMsg->GetSource(); }
    catch ( ... ) { }

    RaiseEvent ( p2pf::P2PF_EVT_ROUTING_ERROR, lpszPeer, lpszWhat );
    return P2PeerHub::On_P2PeerError ( pMsg );
}

///////////////////////////////////////////////////////////////////////
//  Connection lifecycle

void
FacadeHub::MarkPeer ( P2PaddrSTR strPeer, bool bUp )
{
    if ( !strPeer || !*strPeer )
      return;

    bool bChanged = false;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      bool& rState = m_mapPeers[strPeer];
      bChanged = ( rState != bUp );
      rState   = bUp;
    }

    if ( !bChanged || !m_pEvents )
      return;

    try
    {
      if ( bUp ) m_pEvents->OnPeerUp   ( strPeer );
      else       m_pEvents->OnPeerDown ( strPeer );
    }
    catch ( ... ) { }
}

//  Listening side of a completed login
conRESULT
FacadeHub::On_ConLogin ( P2PeerCon *pCon, P2PaddrSTR strThatP2Paddr
                       , const void *pvLoginMsg, P2Psize_t iSize )
{
    conRESULT r = P2PeerHub::On_ConLogin ( pCon, strThatP2Paddr, pvLoginMsg, iSize );
    MarkPeer ( strThatP2Paddr, true );
    return r;
}

//  Dialing side of a completed login
conRESULT
FacadeHub::On_ConLoginAck ( P2PeerCon *pCon
                          , P2PaddrSTR strThisP2Paddr, P2PaddrSTR strThatP2Paddr
                          , const void *pvLoginAck, P2Psize_t iSize )
{
    conRESULT r = P2PeerHub::On_ConLoginAck ( pCon, strThisP2Paddr, strThatP2Paddr
                                            , pvLoginAck, iSize );
    MarkPeer ( strThatP2Paddr, true );
    return r;
}

conRESULT
FacadeHub::On_ConClose ( P2PeerCon *pCon )
{
    // Read everything off the connection BEFORE delegating: the base handler
    // may retire the object.
    //
    // The second read is the one close that would otherwise say NOTHING. A
    // derived Dmx dial that has spent its rendezvous budget stops here for
    // good, and the client has had no event about it at all: Connect returned
    // S_OK when the dial was armed, and the peer never logged in, so MarkPeer
    // has no state to change and no OnPeerDown can fire. Retrying eight times
    // and then going quiet would only have turned "silently lost once" into
    // "silently lost eight times" -- the silence was always the worse half of
    // this defect. Reported after the delegate, with the same try/catch
    // discipline as every other client callback.
    CString csPeer, csService;
    bool    bGaveUp = false;
    try
    {
      if ( pCon )
      {
        csPeer = (P2PaddrSTR)pCon->GetP2Paddress();

        RetryDialDmx *pDial = dynamic_cast<RetryDialDmx*> ( pCon );
        if ( pDial && pDial->Exhausted() )
        {
          bGaveUp   = true;
          csService = pDial->ServiceName();
        }
      }
    }
    catch ( ... ) { }

    // MUST delegate: the base handler is what Restart()s an auto-restart
    // connection, i.e. the whole redial loop hangs off it.
    conRESULT r = P2PeerHub::On_ConClose ( pCon );

    if ( !csPeer.IsEmpty() )
      MarkPeer ( (P2PaddrSTR)csPeer, false );

    if ( bGaveUp )
    {
      CString csWhat;
      csWhat.Format ( L"TargetFacade: gave up dialing '%s' after %d attempts "
                      L"at 'dmx://%s'. The in-process listener never armed, or "
                      L"the hub that owned it has closed."
                    , (LPCWSTR)csPeer, kRedialDmxTries, (LPCWSTR)csService );
      RaiseEvent ( p2pf::P2PF_EVT_DIAL_GAVE_UP, csPeer, csWhat );
    }
    return r;
}

///////////////////////////////////////////////////////////////////////
//  ABI 6 -- past the messaging slice
//
//  Everything below reaches something the facade could always DO and never
//  say.  See missing.md for the audit these came from, and note the shape they
//  share: three of them (SetTimer, Post, Ping) are client-thread calls whose
//  work has to happen on the pump, and all three get there the same way -- one
//  self-addressed P2PeerMsg under a "P2PF$" topic, picked up by HandlePrivate.
//  Marshalling through the kernel's own ordered queue rather than through a
//  lock is what makes them safe to call from anywhere.

//
//  Drop ONE peer
//  NOTES: The machinery is CloseCon, which Link has used to unwind a
//         half-armed edge since ABI 2.  What was missing was a public caller:
//         Close() is whole-hub, so "stop talking to that peer" meant tearing
//         down every other connection with it
//       : DESTROY, not CLOSE -- CloseCon's own note.  A facade dial sets
//         m_uAutoRestart, so a close would merely start the redial chain
//       : The pump-thread guard is not defensive dressing.  CloseCon waits for
//         the connection to leave the hub's list, and the thread that removes
//         it is the pump -- so calling this from a callback would block the
//         only thread that could satisfy the wait, for the whole budget, every
//         time
//       : ...WHICH IS WHY A CALLER-PUMPED HUB IS EXEMPT (ABI 9).  There the
//         waiter and the pump are the same thread, so CloseCon's wait DRIVES
//         the pump instead of blocking it (WaitSlice), and the deadlock the
//         guard exists to prevent cannot arise.  The guard is against a
//         SPAWNED hub's pump thread specifically, not against the pump thread
//         in general -- it always was, and only now is there a difference
//
HRESULT
FacadeHub::Disconnect ( const wchar_t *peer )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !peer )  return E_POINTER;
    if ( !*peer ) return E_INVALIDARG;
    if ( !m_bCallerPumped && OwnsPump() )
      return p2pf::P2PF_E_PUMP_THREAD;

    BOOL bExists = FALSE;
    try { bExists = ConExists ( peer ); }
    catch ( ... ) { return p2pf::P2PF_E_CLOSED; }
    if ( !bExists )
      return p2pf::P2PF_E_NO_PEER;

    bool bWasUp = IsPeerUp ( peer ) ? true : false;
    BOOL bGone  = CloseCon ( peer, FacadeHub::kRetireSlices );

    // Only if it WAS up: MarkPeer would otherwise insert a row for a peer this
    // hub never saw come up, and the read side would then list it forever.
    if ( bWasUp )
      MarkPeer ( peer, false );

    // A connection that did not leave inside the budget is STILL ON THIS HUB,
    // and the two lines above have just put its peer into the one state
    // DrainCons deliberately skips: CloseCon erased the armed record, and
    // MarkPeer wrote "was up, now down", which the drain reads as REDIALLING
    // and leaves alone.  So a timed-out Disconnect used to be worse than no
    // Disconnect at all -- Close() then handed the kernel a hub that still
    // owned a connection, which is precisely the teardown DrainCons exists to
    // prevent (measured: ASSERT(!pOVERLAPPEDcon->bQueued) in
    // P2PeerCon::PostOVERLAPPED, i.e. the process aborts on the close).
    // Remembering it here is what lets the drain finish the job.
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      if ( bGone ) m_setUnretired.erase  ( peer );
      else         m_setUnretired.insert ( peer );
    }

    return bGone ? S_OK : S_FALSE;
}

HRESULT
FacadeHub::SetExtEvents ( p2pf::IP2PHubEvents2 *events2 )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    CSingleLock oLock ( &m_oCSectPeers, TRUE );
    m_pExtEvents = events2;
    return S_OK;
}

//
//  Arm a one-shot timer that fires on the pump thread
//  NOTES: TWO IDs, and the second one is the reason this is not a one-liner.
//         The kernel's PITimerID does not exist until SetPITimer runs, and
//         SetPITimer can only run on the pump (pump 0 means "the pump on this
//         thread").  So the facade hands out its own id immediately, records
//         the row, and lets the pump fill in the kernel id when it arms
//       : The row is written BEFORE the message is posted, for the same reason
//         ArmRecorded writes its record before posting a connection: the pump
//         can arm and even FIRE the timer while this thread is still between
//         the two statements, and a callback that arrives for an id the map
//         has never heard of is indistinguishable from a stray
//       : P2PF_E_NO_SINK rather than a timer that fires into nothing.  OnTimer
//         lives on the extended sink, so without one there is no way to
//         deliver, and failing at the call beats failing silently later
//       : The DEADLINE is stamped HERE, on the caller's thread, and everything
//         downstream works towards it (ArmPumpTimer, On_PITimer).  Stamping it
//         at arming time instead would charge the client for the facade's own
//         trip through the pump queue
//
HRESULT
FacadeHub::SetTimer ( unsigned int delayMillisecs, unsigned int key
                    , unsigned int *outTimerId )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outTimerId ) return E_POINTER;
    *outTimerId = 0;
    if ( !ExtEvents() )
      return p2pf::P2PF_E_NO_SINK;

    unsigned int uId = 0;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      uId = ++m_uNextTimerId;
      TimerRow oRow = { 0, key, ::GetTickCount64() + delayMillisecs, 0 };
      m_mapTimers[uId] = oRow;
    }

    PrivTimerBody oBody;
    oBody.uTimerId = (UINT32)uId;

    HRESULT hr = PostMsg ( (LPCWSTR)m_csAddress, kTopicTimer
                         , &oBody, sizeof(oBody) );
    if ( FAILED(hr) )
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      m_mapTimers.erase ( uId );
      return hr;
    }

    *outTimerId = uId;
    return S_OK;
}

HRESULT
FacadeHub::KillTimer ( unsigned int timerId )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      if ( m_mapTimers.find ( timerId ) == m_mapTimers.end() )
        return S_FALSE;                // unknown, or it has already fired
    }

    // The row is left for the pump to erase, because only the pump knows
    // whether the kernel timer was armed yet -- and only the pump may kill it.
    PrivKillBody oBody;
    oBody.uTimerId = (UINT32)timerId;
    return PostMsg ( (LPCWSTR)m_csAddress, kTopicKill, &oBody, sizeof(oBody) );
}

HRESULT
FacadeHub::Post ( unsigned int key, void *context )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !ExtEvents() )
      return p2pf::P2PF_E_NO_SINK;

    PrivPostBody oBody;
    oBody.uKey     = (UINT32)key;
    oBody.uContext = (UINT64)(UINT_PTR)context;
    return PostMsg ( (LPCWSTR)m_csAddress, kTopicPost, &oBody, sizeof(oBody) );
}

//
//  Arm the kernel timer for one row, on the pump thread
//  NOTES: TWO FORMS, and which one is used is the whole of the timer fix --
//         see the block at the top of this file.
//           - PLAIN (bCompensate false), for the first arm.  The kernel's own
//             arithmetic is exact whenever the pump's next wake-up is this
//             timer's own sleep, which is the quiet case, and compensating it
//             there would cost up to a second of lateness for nothing;
//           - COMPENSATED (true), for a re-arm.  A re-arm happens only because
//             the plain form DID fire early, i.e. something else is waking the
//             pump -- and adding back the position within the second puts the
//             kernel's truncated deadline on the facade's real one, so this
//             form cannot be early whatever wakes it.  One is therefore always
//             enough
//       : What neither form can close is the LATE side: the pump notices an
//         elapsed timer only when it next wakes, so a timer can be up to about
//         a second late.  That is the kernel's own poll granularity, and late
//         is what every timer API in the world does
//       : A deadline already past arms with 0, so an overdue timer fires at
//         the next poll rather than waiting out the second it is already in
//
PITimerID
FacadeHub::ArmPumpTimer ( ULONGLONG uDeadlineTick, unsigned int uTimerId
                        , bool bCompensate )
{
    ULONGLONG  uNow   = ::GetTickCount64();
    P2Pmsecs_t nDelay = 0;
    if ( uDeadlineTick > uNow )
    {
      nDelay = (P2Pmsecs_t)( uDeadlineTick - uNow );
      if ( bCompensate )
        nDelay += (P2Pmsecs_t)MillisecsIntoSecond();
    }

    try   { return SetPITimer ( nDelay, (DWORD)uTimerId ); }
    catch ( ... ) { return 0; }
}

//
//  Timer elapsed, on the pump thread
//  NOTES: dwUserKey carries the FACADE's id, not the client's key -- that is
//         what the row is looked up by, and the client's key comes back out of
//         the row.  Using the client key as the kernel's user key instead
//         would make two timers with the same key indistinguishable
//       : An id the map does not know is handed to the base handler, which
//         reports it.  That keeps the kernel's own diagnostic for a genuinely
//         stray timer instead of swallowing it here
//       : THE ROW IS NOT ERASED UNTIL THE TIMER IS ACTUALLY SPENT.  This is
//         where "never early" is enforced: a callback that arrives before the
//         facade's own deadline is re-armed for what remains instead of being
//         delivered.  The kernel cannot enforce it -- its deadline is
//         truncated to the second, and any other wake-up of the pump then
//         reads it as already past (see the block at the top of this file)
//       : The re-arm is COMPENSATED, so it cannot be early in turn; one is
//         therefore always enough, and kTimerMaxRearms is a backstop against a
//         stepped clock rather than a loop bound
//
void
FacadeHub::On_PITimer ( bool bCancel, PITimerID nTimerID, DWORD dwUserKey )
{
    unsigned int uKey    = 0;
    bool         bKnown  = false;
    ULONGLONG    uRetry  = 0;         // non-zero: re-arm for this deadline
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      std::map<unsigned int,TimerRow>::iterator it
        = m_mapTimers.find ( (unsigned int)dwUserKey );
      if ( it != m_mapTimers.end() )
      {
        uKey   = it->second.uKey;
        bKnown = true;

        if ( !bCancel                                &&
             ::GetTickCount64() < it->second.uDeadline &&
             it->second.uRearms < kTimerMaxRearms       )
        {
          ++it->second.uRearms;
          uRetry = it->second.uDeadline;
        }
        else
          m_mapTimers.erase ( it );    // one shot
      }
    }

    if ( !bKnown )
    {
      P2PeerTarget::On_PITimer ( bCancel, nTimerID, dwUserKey );
      return;
    }

    if ( uRetry )
    {
      // Compensated: this arm exists BECAUSE the plain one fired early, so it
      // must not be able to do so again.
      PITimerID nKernel = ArmPumpTimer ( uRetry, (unsigned int)dwUserKey, true );

      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      std::map<unsigned int,TimerRow>::iterator it
        = m_mapTimers.find ( (unsigned int)dwUserKey );
      if ( it == m_mapTimers.end() )
      {
        // KillTimer ran while this frame was between the two locks.
        if ( nKernel )
          try { ::KillP2PmsgTimer ( nKernel ); } catch ( ... ) { }
      }
      else if ( nKernel )
        it->second.nKernelID = nKernel;
      else
        m_mapTimers.erase ( it );      // could not re-arm: forget it
      return;
    }

    if ( bCancel )
      return;                          // retracted, not elapsed

    p2pf::IP2PHubEvents2 *pExt = ExtEvents();
    if ( pExt )
    {
      try { pExt->OnTimer ( (unsigned int)dwUserKey, uKey ); }
      catch ( ... ) { }
    }
}

//
//  The connection for one peer that answers the question being asked
//  NOTES: A NAMED listen leaves TWO connections on this hub answering to the
//         same peer address, and the difference between them matters.
//         AcceptSpawn (P2PeerCon.cpp:221-256) posts the accepted clone onto
//         the SAME hub and copies m_oThatP2Paddr from the listener, so:
//           - the SERVICE this hub armed keeps waiting for the next peer.  It
//             never logs in, and AcceptSpawn handed the clone a Clone() of its
//             P2Peerio -- so its protocol object carries no traffic at all;
//           - the CLONE completes the login (P2PeerCon.cpp:1889-1893, which
//             also rewrites its address to the claimed name) and is the object
//             actually sending, receiving and holding the session
//       : P2PeerHub::ConQuery returns the FIRST match, and the service was
//         posted at arm time, so it wins.  Every per-connection option
//         therefore used to address the idle object on a listening hub:
//         ConState had no Login, and a trace switched on there switched on
//         nothing.  A WILDCARD listen never had the problem -- the service
//         answers to the pattern and only the clone answers to the claimed
//         name -- which is why it took a named listener to surface it
//       : Choosing needs the whole list, which is why EnumP2PmsgCon is now
//         exported from TargetCore (P2Pwin32.h).  ConQuery cannot express it:
//         it has no notion of a better match
//       : The hub's own m_oCSectionHub is held across the walk, exactly as
//         ConQuery does it, and the SafeP2PeerCon assignment (which AddRefs)
//         happens INSIDE that lock -- so the connection cannot be retired
//         between being chosen and being referenced
//
BOOL
FacadeHub::FindCon ( const wchar_t *peer, ConPick ePick
                   , SafeP2PeerCon& rSafe ) const
{
    struct HubLock
    {
        CRITICAL_SECTION *p;
        HubLock ( CRITICAL_SECTION& cs ) : p ( &cs ) { ::EnterCriticalSection ( p ); }
       ~HubLock ( )                                  { ::LeaveCriticalSection ( p ); }
    };

    FacadeHub *pThis  = const_cast<FacadeHub*> ( this );
    P2PeerCon *pFirst = 0;
    P2PeerCon *pBest  = 0;

    HubLock oLock ( pThis->m_oCSectionHub );
    try
    {
      P2PeerCon *pCon = 0;
      while ( ::EnumP2PmsgCon ( pThis->m_nHubID, &pCon ) && pCon )
      {
        if ( !( pCon->GetP2Paddress() == peer ) )
          continue;

        if ( !pFirst )
          pFirst = pCon;

        bool bWanted = ( ePick == pickSession )
                     ?   pCon->HasState ( ConState_Login )
                     : ( pCon->GetMode() != P2PeerCon_Accept );
        if ( bWanted && !pBest )
          pBest = pCon;
      }
    }
    catch ( ... )
    {
      // The hub context is gone (closed, or closing on its own thread).
      // Whatever was found before the throw is still valid; nothing was.
    }

    // Fall back to the first match: before login there IS no session
    // connection, and the armed one is then the only truthful answer.
    if ( pBest || pFirst )
      rSafe = pBest ? pBest : pFirst;
    return rSafe ? TRUE : FALSE;
}

//
//  One P2Peerio knob on one connection
//  NOTES: MODE is deliberately the ONE option answered from the armed
//         connection rather than the session one.  It reports what THIS HUB
//         DID -- listen or dial -- which is the same fact the read side
//         reports as P2PF_CON_LISTEN / P2PF_CON_DIAL, and the two must not
//         disagree.  Answering it from the session connection would report
//         P2PF_CONMODE_ACCEPTED on every named listener, which is true of the
//         object and useless as an answer to "what did I do here"
//       : Everything else is about the LIVE LINK -- its state bits, its trace
//         switch, its size limits, whether a crypto provider is attached -- so
//         it goes to the connection carrying the session
//       : The two connection-level options are answered before the protocol
//         object is fetched, because a connection always has state and a mode
//         and does not always have a P2Peerio
//
HRESULT
FacadeHub::ConOption ( const wchar_t *peer, unsigned int option
                     , unsigned int *pValue, bool bSet ) const
{
    if ( !peer || !pValue ) return E_POINTER;
    if ( !*peer )           return E_INVALIDARG;

    SafeP2PeerCon oSafe;
    ConPick ePick = ( option == p2pf::P2PF_OPT_MODE ) ? pickArmed : pickSession;
    if ( !FindCon ( peer, ePick, oSafe ) )
      return p2pf::P2PF_E_NO_PEER;

    try
    {
      switch ( option )
      {
        case p2pf::P2PF_OPT_CONSTATE:
          if ( bSet ) return p2pf::P2PF_E_OPTION;
          *pValue = (unsigned int)oSafe->GetState ( ~0u );
          return S_OK;

        case p2pf::P2PF_OPT_MODE:
          if ( bSet ) return p2pf::P2PF_E_OPTION;
          *pValue = (unsigned int)oSafe->GetMode();
          return S_OK;
      }

      P2Peerio *pIo = oSafe->GetP2Peerio();
      if ( !pIo )
        return p2pf::P2PF_E_OPTION;

      switch ( option )
      {
        case p2pf::P2PF_OPT_TRACE:
          if ( bSet ) pIo->SetIFTrace ( *pValue != 0 );
          else        *pValue = pIo->GetIFTrace() ? 1u : 0u;
          return S_OK;

        case p2pf::P2PF_OPT_MAXSEND:
          if ( bSet ) pIo->SetMaxSendSize ( (DWORD)*pValue );
          else        *pValue = (unsigned int)pIo->GetMaxSendSize();
          return S_OK;

        case p2pf::P2PF_OPT_MAXRECV:
          if ( bSet ) pIo->SetMaxRecvSize ( (DWORD)*pValue );
          else        *pValue = (unsigned int)pIo->GetMaxRecvSize();
          return S_OK;

        case p2pf::P2PF_OPT_ENCRYPTED:
          if ( bSet ) return p2pf::P2PF_E_OPTION;
          *pValue = pIo->IsEncrypted() ? 1u : 0u;
          return S_OK;
      }
    }
    catch ( ... )
    {
      return p2pf::P2PF_E_OPTION;
    }
    return p2pf::P2PF_E_OPTION;
}

HRESULT
FacadeHub::SetConOption ( const wchar_t *peer, unsigned int option
                        , unsigned int value )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    return ConOption ( peer, option, &value, true );
}

HRESULT
FacadeHub::GetConOption ( const wchar_t *peer, unsigned int option
                        , unsigned int *outValue ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );
    return ConOption ( peer, option, outValue, false );
}

//
//  Round-trip probe
//  NOTES: NOT the kernel's P2PmsgPing.  That message has no responder at all
//         -- P2PeerTarget::On_MsgPing raises P2Pevent_UNKNOWN and returns
//         msgCONTINUE -- so the answer has to come from the facade at the far
//         end, on a private topic.  A peer that is not a facade hub simply
//         times out, which is the honest outcome
//       : It rides ordinary routing, so a peer several hops away can be
//         pinged.  What comes back is the whole path, not one link
//       : The waiter owns the event handle and is the only thing that removes
//         its own row.  The pump only ever fills in the answer and signals, so
//         it can never touch a handle this frame has closed
//       : SAFE ACROSS Close(), which it was not.  Close ends in `delete this`,
//         and a waiter parked here is a client thread that would then touch a
//         freed hub -- so CloseInternal marks, releases and then WAITS for
//         this frame to leave, and a ping begun after the mark is refused with
//         P2PF_E_CLOSED rather than registered.  See CloseInternal
//
HRESULT
FacadeHub::Ping ( const wchar_t *peer, unsigned int timeoutMillisecs
                , unsigned int *outMillisecs )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !peer || !outMillisecs ) return E_POINTER;
    if ( !*peer )                 return E_INVALIDARG;
    if ( !m_bCallerPumped && OwnsPump() )
      return p2pf::P2PF_E_PUMP_THREAD;

    *outMillisecs = 0;
    unsigned int uBudget = timeoutMillisecs ? timeoutMillisecs
                                            : kPingDefaultMillisecs;
    if ( uBudget > kPingMaxMillisecs )
      uBudget = kPingMaxMillisecs;

    HANDLE hEvent = ::CreateEvent ( 0, TRUE, FALSE, 0 );
    if ( !hEvent )
      return E_OUTOFMEMORY;

    unsigned int uId = 0;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );

      // The other half of CloseInternal's handshake. Registering here after
      // the close has released the waiters would leave a row nothing will ever
      // signal, on an object about to be freed -- so past that point a ping is
      // refused rather than begun.
      if ( m_bClosing )
      {
        ::CloseHandle ( hEvent );
        return p2pf::P2PF_E_CLOSED;
      }

      uId = ++m_uNextPingId;
      PingRow oRow = { hEvent, ::GetTickCount64(), 0, false };
      m_mapPings[uId] = oRow;
    }

    PrivPingBody oBody;
    oBody.uPingId = (UINT32)uId;

    HRESULT hr = PostMsg ( peer, kTopicPing, &oBody, sizeof(oBody) );
    if ( SUCCEEDED(hr) )
    {
      // On a spawned hub the answer arrives on another thread and this one
      // simply parks.  On a caller-pumped hub owned by THIS thread there is no
      // other thread: the answer can only arrive from inside the pump, so the
      // budget is spent running it a slice at a time.  Same budget, same
      // outcome, and the only shape in which the outcome is reachable at all.
      if ( m_bCallerPumped && OwnsPump() )
      {
        ULONGLONG uUntil = ::GetTickCount64() + uBudget;
        for ( ;; )
        {
          if ( ::WaitForSingleObject ( hEvent, 0 ) == WAIT_OBJECT_0 )
            { hr = S_OK; break; }
          if ( ::GetTickCount64() >= uUntil )
            { hr = p2pf::P2PF_E_TIMEOUT; break; }
          WaitSlice ( kDrainSliceMillisecs );
        }
      }
      else
        hr = ( ::WaitForSingleObject ( hEvent, uBudget ) == WAIT_OBJECT_0 )
           ? S_OK : p2pf::P2PF_E_TIMEOUT;
    }

    unsigned int uMillisecs = 0;
    bool         bAnswered  = false;
    {
      CSingleLock oLock ( &m_oCSectPeers, TRUE );
      std::map<unsigned int,PingRow>::iterator it = m_mapPings.find ( uId );
      if ( it != m_mapPings.end() )
      {
        uMillisecs = it->second.uMillisecs;
        bAnswered  = it->second.bAnswered;
        m_mapPings.erase ( it );
      }
    }
    ::CloseHandle ( hEvent );

    // The event is also set by CloseInternal, which is not an answer.
    if ( SUCCEEDED(hr) && !bAnswered )
      return p2pf::P2PF_E_TIMEOUT;
    if ( SUCCEEDED(hr) )
      *outMillisecs = uMillisecs;
    return hr;
}

//
//  Ask idle connections to close
//  NOTES: This is the kernel's PauseHub, under the name of what it actually
//         does: it signals P2PsigCon_CLOSEONIDLE to every connection
//         (P2PeerHub.cpp:228-243).  It is safe from any thread -- from a
//         client thread it posts P2PsigHub_PAUSE and the pump performs it
//       : There is deliberately no counterpart.  P2PeerHub::WakeupHub is an
//         ASSERT(0) stub (P2PeerHub.cpp:248-262) reached through the same
//         signal path, so exposing a `resume` would put a debug assertion in
//         the client's way and do nothing in Release
//
HRESULT
FacadeHub::CloseIdleCons ( )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    try { PauseHub(); }
    catch ( ... ) { return p2pf::P2PF_E_CLOSED; }
    return S_OK;
}

//
//  The escape hatch
//  NOTES: One cast is all that separates a client from the whole kernel, and
//         that is the point -- see the header.  The facade keeps no record of
//         what is done through it and makes no promise about any of it
//       : Returned as void* rather than P2PeerHub* so that this header stays
//         the only header a client that does NOT want the kernel has to
//         include.  A client that does want it already has P2PeerHub.h
//
HRESULT
FacadeHub::GetNative ( void **outNativeHub ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outNativeHub ) return E_POINTER;
    P2PeerHub *pHub = const_cast<FacadeHub*> ( this );
    *outNativeHub   = (void*)pHub;
    return S_OK;
}
