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
// FacadeNetwork.cpp -- kernel lifecycle (real) + hub creation (stub).
//
// The ONE init the public API promises: P2PF_CreateNetwork does
// StartupP2Pmsg + WSAStartup exactly once; the last Release undoes both.
#include "stdafx.h"
#include "FacadeInternal.h"
#include "FacadeHub.h"
#include "FacadeMessage.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

FacadeNetwork    *FacadeNetwork::s_pInstance = 0;
CCriticalSection  FacadeNetwork::s_oCSectInstance;

// ---------------------------------------------------------------------------
// Exported factory
// ---------------------------------------------------------------------------
extern "C" P2PF_API HRESULT __stdcall
P2PF_CreateNetwork ( unsigned int abiVersion, p2pf::IP2PNetwork **outNetwork )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outNetwork )
        return E_POINTER;
    *outNetwork = 0;

    // A RANGE, not a single number, and the range starts at the hard cut.
    // ABI 4 removed the eight typed arming verbs and put Listen/Connect in
    // their place, so every slot after the arming pair moved and nothing built
    // against 1-3 can run here at all. From 4 onwards the append-only mechanic
    // is back: ABI 5 added three methods to the END of IP2PNetwork and touched
    // nothing else, so a v4 binary sees an unchanged vtable prefix and works
    // untouched -- it just cannot call the three it does not know about.
    if ( abiVersion < p2pf::ABI_VERSION_MIN ||
         abiVersion > p2pf::ABI_VERSION      )
        return p2pf::P2PF_E_ABI_MISMATCH;

    return FacadeNetwork::Acquire ( outNetwork );
}

// ---------------------------------------------------------------------------
// Singleton acquire / kernel startup
// ---------------------------------------------------------------------------
HRESULT
FacadeNetwork::Acquire ( p2pf::IP2PNetwork **outNetwork )
{
    CSingleLock oLock ( &s_oCSectInstance, TRUE );

    if ( !s_pInstance )
    {
        FacadeNetwork *pNet = new FacadeNetwork;
        if ( !pNet->Startup() )
        {
            pNet->Shutdown();     // roll back whichever half came up
            delete pNet;
            return p2pf::P2PF_E_STARTUP;
        }
        s_pInstance = pNet;
    }

    ::InterlockedIncrement ( &s_pInstance->m_cRef );
    *outNetwork = s_pInstance;
    return S_OK;
}

BOOL
FacadeNetwork::Startup ( )
{
    m_bMsgUp = StartupP2Pmsg ( 16 );
    if ( !m_bMsgUp )
        return FALSE;

    WSADATA wsa;
    m_bWsaUp = ( ::WSAStartup ( MAKEWORD(2,2), &wsa ) == 0 );
    return m_bWsaUp;
}

void
FacadeNetwork::Shutdown ( )
{
    // The diagnostics slot FIRST, and before the hubs: it holds a raw pointer
    // to the client's sink, and every step below this line can raise events.
    // Handing them to a sink whose owner is already unwinding is exactly the
    // crash SetDiagSink(NULL) exists to prevent, so the release path does it
    // for a client that did not.
    SetDiagSink ( 0, 0 );

    CloseAllHubs();
    if ( m_bMsgUp )  { CleanupP2Pmsg();  m_bMsgUp = FALSE; }
    if ( m_bWsaUp )  { ::WSACleanup();   m_bWsaUp = FALSE; }
}

//
//  Close and destroy every hub the client left open
//  NOTES: The list is drained one entry at a time so the lock is never
//         held across a hub teardown -- a pump thread unwinding through
//         On_ConClose wants its own hub lock, and FacadeHub::Close() (if a
//         client races us) wants this one
//
void
FacadeNetwork::CloseAllHubs ( )
{
    for ( ;; )
    {
      FacadeHub *pHub = 0;
      {
        CSingleLock oLock ( &m_oCSection, TRUE );
        if ( m_listHubs.IsEmpty() )
          break;
        pHub = m_listHubs.RemoveHead();
      }
      pHub->CloseInternal();
      delete pHub;
    }
}

//
//  TRUE when a live hub already answers to this address
//  NOTES: Two live hubs sharing one address corrupt the kernel's hub
//         registry: logins on UNRELATED hubs start being refused, and the
//         process dies shortly after (measured, Release and Debug alike).
//         The kernel does not police it, so the facade does -- this is the
//         only registry of live hubs either side has
//       : Case-sensitive, like every other P2Paddr comparison (the
//         case-folding branch in P2Paddr::IsMapped is commented out,
//         P2Peer.cpp:478), so "Demo" and "demo" are two different hubs
//       : Caller MUST hold m_oCSection
//
FacadeHub*
FacadeNetwork::FindHubLocked ( const wchar_t *address )
{
    for ( POSITION pos = m_listHubs.GetHeadPosition(); pos; )
    {
      FacadeHub *pHub = m_listHubs.GetNext ( pos );
      if ( pHub && ::wcscmp ( pHub->Address(), address ) == 0 )
        return pHub;
    }
    return 0;
}

BOOL
FacadeNetwork::IsAddressTakenLocked ( const wchar_t *address )
{
    return FindHubLocked ( address ) != 0;
}

void
FacadeNetwork::RemoveHub ( FacadeHub *pHub )
{
    CSingleLock oLock ( &m_oCSection, TRUE );
    POSITION pos = m_listHubs.Find ( pHub );
    if ( pos )
      m_listHubs.RemoveAt ( pos );
}

// ---------------------------------------------------------------------------
// IP2PNetwork
// ---------------------------------------------------------------------------
HRESULT
FacadeNetwork::CreateHub ( const wchar_t *address
                         , p2pf::IP2PHubEvents *events
                         , p2pf::IP2PHub **outHub )
{
    // ONE implementation, not two: a spawned hub is the flags==0 case of a
    // hub, and duplicating the address guard, the re-check under the publish
    // lock and the unwind would be two chances to fix a bug once.
    return CreateHubEx ( address, events, p2pf::P2PF_HUB_SPAWN_PUMP, outHub );
}

//
//  CreateHub, plus WHOSE THREAD                                       (ABI 9)
//  NOTES: The only difference between the two shapes is one call -- SpawnHub
//         against P2PeerHub::CreateHub -- and it is made in the middle of a
//         sequence whose every other step is identical.  Hence one function
//       : P2PF_E_HUB_SPAWN covers the caller-pumped failure too, and it is the
//         honest code rather than a lazy one: what went wrong is that no pump
//         could be started for this hub.  The commonest cause is specific and
//         worth knowing -- the kernel allows one pump per THREAD, so a thread
//         that already drives a caller-pumped hub, or that is itself some
//         other hub's pump thread, cannot have another
//
HRESULT
FacadeNetwork::CreateHubEx ( const wchar_t *address
                           , p2pf::IP2PHubEvents *events
                           , unsigned int flags
                           , p2pf::IP2PHub **outHub )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !address || !events || !outHub )
        return E_POINTER;
    *outHub = 0;
    if ( !*address )
        return E_INVALIDARG;
    if ( flags & ~p2pf::P2PF_HUB_CALLER_PUMPED )
        return E_INVALIDARG;

    // Reject a duplicate address BEFORE spawning anything: a second live hub
    // on one address is unrecoverable, not merely useless.
    {
      CSingleLock oLock ( &m_oCSection, TRUE );
      if ( IsAddressTakenLocked ( address ) )
        return p2pf::P2PF_E_HUB_DUPLICATE;
    }

    const bool bCallerPumped = ( flags & p2pf::P2PF_HUB_CALLER_PUMPED ) != 0;

    FacadeHub *pHub = new FacadeHub ( address, events, this );
    if ( !( bCallerPumped ? pHub->StartCallerPumped() : pHub->Start() ) )
    {
      delete pHub;
      return p2pf::P2PF_E_HUB_SPAWN;
    }

    {
      CSingleLock oLock ( &m_oCSection, TRUE );

      // Re-check under the lock we publish under: another thread may have
      // taken the address while this hub was spawning. The pump is already up
      // (this thread's, for a caller-pumped hub), so this one has to be torn
      // down properly -- it is not in m_listHubs yet, so CloseInternal (not
      // Close) is the right exit, and it runs on the owning thread either way.
      if ( IsAddressTakenLocked ( address ) )
      {
        oLock.Unlock();
        pHub->CloseInternal();
        delete pHub;
        return p2pf::P2PF_E_HUB_DUPLICATE;
      }

      m_listHubs.AddTail ( pHub );
    }

    *outHub = pHub;   // adjusts to the IP2PHub subobject
    return S_OK;
}

ULONG
FacadeNetwork::Release ( )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    CSingleLock oLock ( &s_oCSectInstance, TRUE );

    LONG cRef = ::InterlockedDecrement ( &m_cRef );
    if ( cRef > 0 )
        return (ULONG)cRef;

    s_pInstance = 0;
    Shutdown();
    delete this;
    return 0;
}

const wchar_t*
FacadeNetwork::VersionString ( ) const
{
    return L"TargetFacade ABI 10 / TargetCore(2022)";
}

///////////////////////////////////////////////////////////////////////
//  In-process resolution (the one place an address IS enough)

//
//  What should `dialerAddr` dial to reach the hub `peerAddr`?
//  NOTES: The network is the only object in the process that holds every
//         live hub, so it is the only one that can answer this at all.  The
//         kernel cannot: its hub registry is ID-keyed with no address lookup,
//         and a connection's endpoint fields are protected with no getters --
//         which is why the answer comes from what the peer hub RECORDED at
//         arm time rather than from the kernel
//       : Only a LISTENER the peer armed for us counts.  If the peer dialed
//         us, dialing it back is not a link, it is a second connection the
//         kernel would refuse as a duplicate peer address
//       : The whole scan, including reading the peer hub's record, completes
//         under m_oCSection.  A FacadeHub* taken from m_listHubs is only
//         valid while that lock is held -- FacadeHub::Close calls RemoveHub
//         and then `delete this`
//
HRESULT
FacadeNetwork::ResolveDial ( const wchar_t *peerAddr, const wchar_t *dialerAddr
                           , FacadeEndpoint& rEp )
{
    if ( !peerAddr || !*peerAddr || !dialerAddr || !*dialerAddr )
      return p2pf::P2PF_E_UNRESOLVED;

    CString csEndpoint;
    {
      CSingleLock oLock ( &m_oCSection, TRUE );
      FacadeHub *pPeer = FindHubLocked ( peerAddr );
      if ( !pPeer )
        return p2pf::P2PF_E_UNRESOLVED;   // not in this process: nothing to derive from
      csEndpoint = pPeer->ArmedListener ( dialerAddr );
    }
    if ( csEndpoint.IsEmpty() )
      return p2pf::P2PF_E_UNRESOLVED;     // present, but not expecting us (yet)

    // The record is a LISTEN form; turn it into the dial that reaches it.
    // Only the tcp form differs, and only in the one field a listener does
    // not have: the host. In-process, that is loopback by construction.
    HRESULT hr = ParseFacadeEndpoint ( csEndpoint, true, rEp );
    if ( FAILED(hr) )
      return hr;
    if ( rEp.eKind == p2pfTcp && rEp.csHost.IsEmpty() )
      rEp.csHost = L"127.0.0.1";
    return S_OK;
}

///////////////////////////////////////////////////////////////////////
//  The configured endpoint map

//
//  Set or clear ONE entry
//  NOTES: An empty endpoint REMOVES the entry rather than storing an empty
//         one.  "" already means "resolve this for me" everywhere else in the
//         ABI, and an entry that said that would be a lookup that succeeds and
//         answers nothing
//
HRESULT
FacadeNetwork::SetEndpoint ( const wchar_t *address, const wchar_t *endpoint )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !address )   return E_POINTER;
    if ( !*address )  return E_INVALIDARG;
    if ( IsSwappedPeerArgument ( address ) )
      return E_INVALIDARG;              // an endpoint in the address slot

    if ( !endpoint || !*endpoint )
    {
      CSingleLock oLock ( &m_oCSection, TRUE );
      m_mapEndpoints.erase ( address );
      return S_OK;
    }

    // Validated HERE, not at the arming verb that consumes it later. Same
    // grammar, same codes -- and the same reason the arming verbs check their
    // own arguments up front: the call site that made the mistake is the only
    // one that can explain it.
    FacadeEndpoint oEp;
    HRESULT hr = ParseFacadeEndpoint ( endpoint, false, oEp );
    if ( FAILED(hr) )
      return hr;
    if ( oEp.eKind == p2pfSerial )
      return p2pf::P2PF_E_ENDPOINT;     // see ParseFacadeEndpointMap's notes

    CSingleLock oLock ( &m_oCSection, TRUE );
    m_mapEndpoints[address] = (LPCWSTR)oEp.Format();
    return S_OK;
}

//
//  Replace the whole map from one block of text
//  NOTES: REPLACE, not merge.  A deployment map is a statement of what the
//         world looks like, so re-reading a configuration file must not leave
//         entries behind from the last one it had -- that is how a removed
//         line keeps working until the next restart
//       : Parsed before the lock is taken and before anything is discarded, so
//         a rejected block leaves the previous map exactly as it was
//
HRESULT
FacadeNetwork::SetEndpointMap ( const wchar_t *text, unsigned int *badLine )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( badLine )
      *badLine = 0;

    FacadeEndpointMap mapNew;
    HRESULT hr = ParseFacadeEndpointMap ( text, mapNew, badLine );
    if ( FAILED(hr) )
      return hr;

    CSingleLock oLock ( &m_oCSection, TRUE );
    m_mapEndpoints.swap ( mapNew );
    return S_OK;
}

//
//  Read one entry back, canonically spelled
//  NOTES: Answers what the map SAYS, never what a hub armed -- those are
//         different questions and there is a different call for the other one
//         (IP2PHub::GetEndpoint).  They agree only when the map is what the
//         resolution used
//
HRESULT
FacadeNetwork::GetEndpointFor ( const wchar_t *address
                              , wchar_t *buf, unsigned int *cch ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !address || !*address )
      return E_INVALIDARG;

    CString csValue;
    {
      CSingleLock oLock ( &m_oCSection, TRUE );
      FacadeEndpointMap::const_iterator it = m_mapEndpoints.find ( address );
      if ( it == m_mapEndpoints.end() )
        return p2pf::P2PF_E_UNRESOLVED;
      csValue = it->second.c_str();
    }
    return CopyOut ( csValue, buf, cch );
}

//
//  Make an empty message                                             (ABI 8)
//  NOTES: On the NETWORK and not on a hub, and it takes nothing and registers
//         nothing, because the object belongs to nobody: it holds no hub, no
//         kernel object and no lock, so there is nothing here to track and
//         nothing for CloseAllHubs to tidy up.  That is the whole point of it
//         being a value -- see IP2PMessage
//
HRESULT
FacadeNetwork::CreateMessage ( p2pf::IP2PMessage **outMessage )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outMessage )
      return E_POINTER;
    *outMessage = 0;

    FacadeMessage *pMsg = new FacadeMessage;
    if ( !pMsg )
      return E_OUTOFMEMORY;

    *outMessage = pMsg;
    return S_OK;
}

HRESULT
FacadeNetwork::LookupEndpoint ( const wchar_t *address
                              , FacadeEndpoint& rEp ) const
{
    if ( !address || !*address )
      return p2pf::P2PF_E_UNRESOLVED;

    CString csValue;
    {
      CSingleLock oLock ( &m_oCSection, TRUE );
      FacadeEndpointMap::const_iterator it = m_mapEndpoints.find ( address );
      if ( it == m_mapEndpoints.end() )
        return p2pf::P2PF_E_UNRESOLVED;
      csValue = it->second.c_str();
    }

    // Stored canonically and validated on the way in, so this cannot fail --
    // but a parse that "cannot fail" is exactly the one worth checking, since
    // the alternative is arming a half-built FacadeEndpoint.
    return ParseFacadeEndpoint ( (LPCWSTR)csValue, false, rEp );
}

///////////////////////////////////////////////////////////////////////
//  Link -- arm both ends of one edge

//
//  Arm the listening side and then the dialing side of one in-process edge
//  NOTES: Everything that CAN be checked before anything is manufactured is
//         checked first -- unknown address, self-link, swapped argument,
//         unparseable endpoint, and a peer either hub already has a
//         connection for.  Those are the realistic failures, and for all of
//         them nothing is armed at all
//       : If the dialer still fails, the listener is retracted.  "Arms both
//         sides" is a promise of atomicity, and a half-armed edge is the one
//         outcome worth ruling out: FacadeHub offers only whole-hub Close(),
//         so no public call could clear it afterwards
//       : The whole operation runs under m_oCSection, including the unwind.
//         Two hub pointers taken from m_listHubs must both stay valid across
//         both arms, and the only thing keeping them valid is this lock.  It
//         does cost: a concurrent CreateHub/Close blocks for the duration,
//         and on the unwind path that duration includes a bounded wait.  That
//         is the right trade for a call that is made at startup
//
HRESULT
FacadeNetwork::Link ( const wchar_t *listenerAddr, const wchar_t *dialerAddr
                    , const wchar_t *endpoint )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !listenerAddr || !dialerAddr )
      return E_POINTER;
    if ( !*listenerAddr || !*dialerAddr )
      return E_INVALIDARG;
    if ( ::wcscmp ( listenerAddr, dialerAddr ) == 0 )
      return E_INVALIDARG;                          // a hub cannot link to itself
    if ( IsSwappedPeerArgument ( listenerAddr ) ||
         IsSwappedPeerArgument ( dialerAddr )      )
      return E_INVALIDARG;                          // an endpoint in an address slot

    // Both endpoints come from one spelling: the caller states the dial, and
    // the listener is that dial with the host removed. Deriving them the
    // other way round is not possible -- a listener does not know the host a
    // dialer will use to reach it.
    FacadeEndpoint oListen, oDial;
    if ( endpoint && *endpoint )
    {
      HRESULT hr = ParseFacadeEndpoint ( endpoint, false, oDial );
      if ( FAILED(hr) )
        return hr;

      // Serial cannot be expressed here and never will be. A null-modem link
      // is two DIFFERENT local devices (COM5 one end, COM6 the other), each
      // opened exclusively; one endpoint for both sides would have the two
      // hubs fight over one port. Refuse it rather than arm something that
      // cannot work -- two separate calls say it properly, one port per side:
      // Listen(peer, "serial://COM5") on one hub, Connect(peer, "serial://6")
      // on the other.
      if ( oDial.eKind == p2pfSerial )
        return p2pf::P2PF_E_ENDPOINT;

      oListen = oDial;
      oListen.csHost.Empty();
    }
    else
    {
      // bDerived, because this pair of endpoints is exactly what bDerived
      // means: the facade worked them out from live process state and no
      // caller typed them, so the dial can only ever be EARLY -- the
      // listener arms on its own pump a moment after this returns. Link
      // arms in the order that works, but ordering the CALLS does not order
      // the two pumps, so the dial still wants RetryDialDmx behind it.
      //
      // Note the map is NOT consulted here. Link's empty-endpoint contract is
      // "an in-process edge between two hubs THIS network owns" -- both ends
      // are in this process by construction, so a configured cross-machine
      // endpoint for either address would be answering a question nobody
      // asked. Listen/Connect with an omitted endpoint are where the map
      // applies.
      oListen.eKind    = p2pfDmx;
      oListen.csName   = DeriveDmxService ( listenerAddr, dialerAddr );
      oListen.bDerived = true;
      oDial            = oListen;
    }

    CSingleLock oLock ( &m_oCSection, TRUE );

    FacadeHub *pListener = FindHubLocked ( listenerAddr );
    FacadeHub *pDialer   = FindHubLocked ( dialerAddr );
    if ( !pListener || !pDialer )
      return p2pf::P2PF_E_NO_HUB;

    // The kernel allows one connection per peer address per hub and reports a
    // second one by rejecting it -- after destroying it. Asking first turns
    // "the listener armed and then the dial was refused" into "nothing
    // happened", which is what a second Link on one pair should be.
    if ( pListener->ConExists ( dialerAddr ) ||
         pDialer  ->ConExists ( listenerAddr ) )
      return p2pf::P2PF_E_CON_DUPLICATE;

    HRESULT hrListen = pListener->ArmResolved ( dialerAddr, oListen, true );
    if ( FAILED(hrListen) )
      return hrListen;                              // nothing armed anywhere

    HRESULT hrDial = pDialer->ArmResolved ( listenerAddr, oDial, false );
    if ( FAILED(hrDial) )
    {
      if ( !pListener->CloseCon ( dialerAddr, FacadeHub::kUnwindSlices ) )
        return p2pf::P2PF_E_LINK_PARTIAL;           // named, not left to be found
      return hrDial;
    }

    return ( hrListen == p2pf::P2PF_S_UNRELATED_LINK ||
             hrDial   == p2pf::P2PF_S_UNRELATED_LINK   )
           ? p2pf::P2PF_S_UNRELATED_LINK : S_OK;
}

///////////////////////////////////////////////////////////////////////
//  ABI 10 -- the kernel's own diagnostic stream
//
//  WHAT THIS IS BUILT ON, because the header the audit pointed at describes
//  something else entirely.
//
//  missing.md 4.5 lists the surface as CreateP2PeventSink / RegP2PeventCmd /
//  EnumP2PeventSink / IsP2PeventReg / PerformP2PeventNotn -- a registry of
//  sinks, each with its own notification mask, in P2Pwin32.h:316-354.  All of
//  it exists, all of it is exported, and NOTHING IN THE KERNEL EVER NOTIFIES
//  IT.  The function that would -- PerformP2PeventNotn -- is defined TWICE
//  (P2Pwin32.cpp:6072 and Msgcorewin32.cpp:5675, each over its own static sink
//  list) and CALLED FROM NOWHERE: its only call sites are in
//  P2Pevent(2Msgcore).cpp, which is not in the build.  A client can create a
//  sink there, set its mask, enumerate it back, and receive nothing forever.
//
//  What actually dispatches an event is P2Pevent::Cancel -- the kernel's
//  ordinary way of disposing of one -- which calls P2PeventPost_HWND, which
//  calls ONE STATIC CALLBACK (Msgexception.cpp:1337-1346) registered through
//  P2Pevent::Register4P2Pevents.  That is the whole notification system: one
//  slot, one key, synchronous, on the raising thread.  What settles it as the
//  intended path rather than a leftover is the kernel's own file logger --
//  MsgexceptionLog::Startup registers there, with its CreateP2PeventSinkdebug
//  line commented out directly beneath (TargetCoreLog.cpp:184-186).
//
//  Everything visible in this ABI follows from that one fact:
//    * it is on the NETWORK -- the slot is process-wide and takes no hub;
//    * there is ONE sink, and taking the slot displaces the kernel's logger;
//    * delivery is SYNCHRONOUS on the raising thread -- there is no pump in
//      the path to marshal to, which is why this is the one callback in the
//      facade that does not arrive on a pump thread;
//    * the mask is filtered HERE, because the slot is handed everything;
//    * the "current record" gate is per-THREAD rather than per-object, because
//      several threads can be inside a delivery at once.
//
//  One more kernel surprise, and this file depends on it.  Cancel's parameter
//  is documented as "notifications flag ... false.. Skip all notifications",
//  and it does no such thing: Msgexception.cpp:83-96 uses it to decide whether
//  to DISPLAY, and calls P2PeventPost_HWND unconditionally.  So Cancel(false)
//  means "notify, but do not print" -- which is exactly what RaiseDiag needs,
//  because Display() of an ERROR in a process with no console window is a
//  task-modal message box on the raising thread.

namespace {

// The record being delivered ON THIS THREAD, or NULL.  Thread-local, and a
// static rather than a member, for the reason the public header gives: this
// callback is re-entrant across threads by construction, so "the current
// record" is a property of the thread and of nothing else.  Msgcore keeps its
// own last-event pointer the same way (Msgcorewin32.cpp's tls_pMsgexception),
// so the technique is the kernel's own.
__declspec(thread) const P2Pevent *tls_pDiagRec = 0;

// Publish/retract it around one client callback, so a handler that throws
// cannot leave the gate open for every later call on this thread.
class DiagRecScope
{
  public:
    DiagRecScope ( const P2Pevent& rEvent )  { tls_pDiagRec = &rEvent; }
   ~DiagRecScope ( )                         { tls_pDiagRec = 0; }
};

// The facade's severity <-> the kernel's P2Pevent_e.  A switch rather than a
// cast, so P2PF_DIAG_* is this ABI's promise: the two agree today, and a kernel
// that renumbered its enum would otherwise silently renumber ours.
unsigned int
DiagClassOf ( P2Pevent_e eClass )
{
    switch ( eClass )
    {
      case P2Pevent_ERROR:   return p2pf::P2PF_DIAG_ERROR;
      case P2Pevent_WARNING: return p2pf::P2PF_DIAG_WARNING;
      case P2Pevent_INFO:    return p2pf::P2PF_DIAG_INFO;
      case P2Pevent_DEBUG:   return p2pf::P2PF_DIAG_DEBUG;
      case P2Pevent_TRACE:   return p2pf::P2PF_DIAG_TRACE;
      case P2Pevent_LOG:     return p2pf::P2PF_DIAG_LOG;
      case P2Pevent_REPORT:  return p2pf::P2PF_DIAG_REPORT;
      case P2Pevent_USER0:   return p2pf::P2PF_DIAG_APP;
      default:               return (unsigned int)eClass;
    }
}

P2Pevent_e
KernelClassOf ( unsigned int uSeverity )
{
    switch ( uSeverity )
    {
      case p2pf::P2PF_DIAG_ERROR:   return P2Pevent_ERROR;
      case p2pf::P2PF_DIAG_WARNING: return P2Pevent_WARNING;
      case p2pf::P2PF_DIAG_INFO:    return P2Pevent_INFO;
      case p2pf::P2PF_DIAG_DEBUG:   return P2Pevent_DEBUG;
      case p2pf::P2PF_DIAG_TRACE:   return P2Pevent_TRACE;
      case p2pf::P2PF_DIAG_LOG:     return P2Pevent_LOG;
      case p2pf::P2PF_DIAG_REPORT:  return P2Pevent_REPORT;
      case p2pf::P2PF_DIAG_APP:     return P2Pevent_USER0;
      default:                      return (P2Pevent_e)uSeverity;
    }
}

// The sink id handed to Register4P2Pevents and echoed back to the callback.
// It is only ever echoed, so any constant does; this one says who took the
// slot when it turns up in a kernel trace.
const P2PeventSinkID kDiagSinkID = 0x50F2;   // "P2PF"

// How long SetDiagSink waits for in-flight deliveries to leave before it lets
// the caller free the sink.  Bounded like every other wait in this product:
// overshooting costs a slow unregister, and there is no correct number of
// milliseconds to hang for.
const int kDiagQuiesceSlices = 200;   // 200 x 1 ms

} // namespace

//
//  The kernel's one static callback slot, on the thread that raised the event
//  NOTES: dwKey is the network that registered.  It is cleared before that
//         object goes away -- Shutdown calls SetDiagSink(0,0) as its first act
//         -- so a non-null key here is a live network
//       : Everything below runs INSIDE P2Pevent::Cancel, which is about to
//         delete the event.  Nothing may be retained
//
void WINAPI
FacadeNetwork::DiagCallback ( P2PeventSinkID nSinkID, DWORD_PTR dwKey
                            , const P2Pevent& rEvent )
{
    UNREFERENCED_PARAMETER ( nSinkID );

    FacadeNetwork *pNet = reinterpret_cast<FacadeNetwork*>( dwKey );
    if ( !pNet )
      return;
    try { pNet->DeliverDiag ( rEvent ); }
    catch ( ... ) { }
}

//
//  Filter one event and hand it to the client
//  NOTES: The re-entrance test is the whole of the loop protection this layer
//         can offer.  A handler that raises an event -- directly, or by making
//         a facade call that fails -- would otherwise be called again from
//         inside itself, on this thread, without end.  RaiseDiag consults the
//         same gate and answers S_FALSE rather than raising at all
//       : m_lDiagBusy is what makes SetDiagSink(NULL) safe to follow with a
//         delete of the sink.  It is raised before the sink pointer is read
//         and dropped after the callback returns, so an unregister that
//         observes zero knows no thread is inside the client's object
//       : No lock anywhere on this path, deliberately.  An event can be raised
//         from inside a facade call that already holds m_oCSection -- every
//         failure path in this file is a candidate -- and a logger that
//         deadlocks the thing it is logging is worse than no logger
//
void
FacadeNetwork::DeliverDiag ( const P2Pevent& rEvent )
{
    // One delivery per thread. Nothing below here may raise.
    if ( tls_pDiagRec )
      return;

    // Before the mask, and before the sink is even read: this counts what the
    // PROCESS raised, so the gap a filtered client sees is the truth about
    // what its filter cost it.  See m_lDiagSeq for why it is not the kernel's
    // own number.
    unsigned int uSeq = (unsigned int)::InterlockedIncrement ( &m_lDiagSeq );

    ::InterlockedIncrement ( &m_lDiagBusy );

    // A plain read of a volatile pointer-sized member: an aligned pointer load
    // is atomic on every platform this builds for, and the store side is an
    // interlocked exchange, so a reader sees the old sink or the new one and
    // never a torn one.
    p2pf::IP2PDiagEvents *pSink = m_pDiagSink;
    unsigned int         uMask  = (unsigned int)m_lDiagMask;

    if ( pSink )
    {
      // The getters are non-const and the kernel hands over a const reference.
      // That is the kernel's inconsistency rather than a hint: every one of
      // them is a read.
      P2Pevent& rEvt = const_cast<P2Pevent&>( rEvent );

      unsigned int uSeverity  = 0;
      LPCTSTR      lpszModule = L"";
      CString      csText;
      bool         bWanted    = false;
      try
      {
        uSeverity = DiagClassOf ( rEvt.GetClass() );
        bWanted   = uSeverity < 32 && ( uMask & ( 1u << uSeverity ) ) != 0;
        if ( bWanted )
        {
          lpszModule = rEvt.GetModule();
          csText     = rEvt.GetMessage();
        }
      }
      catch ( ... ) { bWanted = false; }

      if ( bWanted )
      {
        DiagRecScope oScope ( rEvent );
        try { pSink->OnDiag ( uSeverity, uSeq
                            , lpszModule ? lpszModule : L""
                            , (LPCWSTR)csText ); }
        catch ( ... ) { }
      }
    }

    ::InterlockedDecrement ( &m_lDiagBusy );
}

HRESULT
FacadeNetwork::SetDiagSink ( p2pf::IP2PDiagEvents *sink, unsigned int mask )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    // The kernel reserves the top bit of every notification mask it has
    // (P2Pevotn_FULL is ~0 & ~(1<<31)), so a client that sets it is asking for
    // a class that cannot exist rather than for everything.
    if ( mask & 0x80000000u )
      return E_INVALIDARG;

    ::InterlockedExchange ( &m_lDiagMask, (LONG)mask );

    // Publish the sink BEFORE taking the slot, and clear it BEFORE giving the
    // slot back, so the window in either direction delivers to nothing rather
    // than to something half-installed.
    // Cast to void**, not void* volatile*: on x64 InterlockedExchangePointer is
    // an intrinsic that accepts the volatile-qualified form, but on x86 it is
    // declared void* InterlockedExchangePointer(void**, void*), which rejects it
    // (C2664). The 32-bit leg is the one the COM server needs.
    ::InterlockedExchangePointer ( (void**)&m_pDiagSink, sink );

    if ( sink )
    {
      // ~0u for the kernel's filter argument because it is not read: the slot
      // records it (Msgexception.cpp:131-139) and P2PeventPost_HWND never
      // consults it. The mask that matters is ours, applied in DeliverDiag.
      P2Pevent::Register4P2Pevents ( kDiagSinkID, &FacadeNetwork::DiagCallback
                                   , ~0u, reinterpret_cast<DWORD_PTR>(this) );
      ::InterlockedExchange ( &m_lDiagHeld, 1 );
      return S_OK;
    }

    // Give the slot back, but only if we ever took it: writing 0 into a slot
    // some other component has since claimed would switch ITS logging off.
    // There is no way to detect that case -- the slot cannot be read back --
    // and not clobbering what we never held is as far as this can be taken.
    if ( ::InterlockedExchange ( &m_lDiagHeld, 0 ) )
      P2Pevent::Register4P2Pevents ( (P2PeventSinkID)0, (P2PeventCBFnc)0
                                   , 0, (DWORD_PTR)0 );

    // Let any delivery already inside the client's object leave before the
    // caller frees it. Skipped when the caller is ITSELF inside one, because
    // then the count includes this thread and could never reach zero -- a
    // client unregistering from its own handler gets an answer, not a hang.
    if ( !tls_pDiagRec )
      for ( int nSlice = 0; nSlice < kDiagQuiesceSlices; ++nSlice )
      {
        if ( m_lDiagBusy == 0 )
          break;
        ::Sleep ( 1 );
      }

    return S_OK;
}

HRESULT
FacadeNetwork::SetDiagMask ( unsigned int mask )
{
    if ( mask & 0x80000000u )
      return E_INVALIDARG;

    ::InterlockedExchange ( &m_lDiagMask, (LONG)mask );

    // S_FALSE rather than a failure: the mask is recorded either way, so a
    // client that widens its filter before arming a sink gets what it asked
    // for, and one that never arms a sink has not done anything wrong.
    return m_pDiagSink ? S_OK : S_FALSE;
}

HRESULT
FacadeNetwork::GetDiagMask ( unsigned int *outMask ) const
{
    if ( !outMask )
      return E_POINTER;

    *outMask = m_pDiagSink ? (unsigned int)m_lDiagMask : 0;
    return S_OK;
}

HRESULT
FacadeNetwork::GetDiagText ( unsigned int part
                           , wchar_t *buf, unsigned int *cch ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !cch )
      return E_POINTER;

    // The gate, and the reason it reads a thread-local rather than a member:
    // asking outside a delivery must not be answered with another thread's
    // record.
    const P2Pevent *pRec = tls_pDiagRec;
    if ( !pRec )
      return p2pf::P2PF_E_NO_DIAG;

    P2Pevent& rEvt = const_cast<P2Pevent&>( *pRec );
    CString   csOut;
    try
    {
      switch ( part )
      {
        case p2pf::P2PF_DIAGT_MESSAGE: csOut = rEvt.GetMessage();     break;
        case p2pf::P2PF_DIAGT_MODULE:  csOut = rEvt.GetModule();      break;
        case p2pf::P2PF_DIAGT_ADVICE:  csOut = rEvt.GetAdvice();      break;
        case p2pf::P2PF_DIAGT_SERVICE: csOut = rEvt.GetService();     break;
        case p2pf::P2PF_DIAGT_GROUP:   csOut = rEvt.GetGroup();       break;
        case p2pf::P2PF_DIAGT_CLASS:   csOut = rEvt.GetClassText();   break;
        case p2pf::P2PF_DIAGT_HRESULT: csOut = rEvt.GetHRESULText();  break;
        default:                       return E_INVALIDARG;
      }
    }
    catch ( ... ) { csOut.Empty(); }

    return CopyOut ( csOut, buf, cch );
}

HRESULT
FacadeNetwork::GetDiagInfo ( unsigned int *outHResult
                           , unsigned int *outTime
                           , unsigned int *outThreadId ) const
{
    const P2Pevent *pRec = tls_pDiagRec;
    if ( !pRec )
      return p2pf::P2PF_E_NO_DIAG;

    P2Pevent& rEvt = const_cast<P2Pevent&>( *pRec );
    try
    {
      if ( outHResult ) *outHResult = (unsigned int)rEvt.GetHRESULT();
      if ( outTime )    *outTime    = (unsigned int)rEvt.GetTime();
    }
    catch ( ... )
    {
      if ( outHResult ) *outHResult = 0;
      if ( outTime )    *outTime    = 0;
    }

    // Not stored anywhere, because it cannot be anything else: the callback
    // runs on the raising thread, so those two ARE one thread and there is no
    // second number to keep.
    if ( outThreadId )  *outThreadId = (unsigned int)::GetCurrentThreadId();
    return S_OK;
}

HRESULT
FacadeNetwork::RaiseDiag ( unsigned int severity
                         , const wchar_t *module, const wchar_t *text )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !text || !*text )
      return E_INVALIDARG;
    if ( severity > 31 )
      return E_INVALIDARG;

    // The one feedback loop this facade can close: a logging handler that logs.
    if ( tls_pDiagRec )
      return S_FALSE;

    try
    {
      P2Pevent *pEVT = P2Pevent::MakeEvent ( KernelClassOf ( severity ) );
      if ( !pEVT )
        return E_FAIL;

      // The UNDERSCORED overloads: they take the string AS a string. Their
      // siblings are printf-style, and handing client text to one of those
      // makes every '%' in a log line a format directive.
      if ( module && *module )
        pEVT->Module_ ( module );
      pEVT->Message_ ( text );

      // false == do not DISPLAY. See this section's header note: the flag does
      // not mean what its name says. Notification happens either way, which is
      // the half wanted here.
      pEVT->Cancel ( false );
      return S_OK;
    }
    catch ( P2Pevent *pEVT )
    {
      try { pEVT->Cancel ( false ); } catch ( ... ) { }
      return E_FAIL;
    }
    catch ( ... ) { return E_FAIL; }
}

HRESULT
FacadeNetwork::IsDiagWanted ( unsigned int mask, unsigned int *outMatched ) const
{
    if ( !outMatched )
      return E_POINTER;

    *outMatched = m_pDiagSink ? ( mask & (unsigned int)m_lDiagMask ) : 0;
    return S_OK;
}
