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
// WildcardListenTest.cpp
//
// What does `toPeer` mean on Listen, and can one listener serve many peers?
//
// `toPeer` is NOT this hub's own address -- that is fixed at CreateHub and is
// never passed again. It is the address of the hub on the OTHER end, and the
// kernel stamps it into TWO places (P2PeerConWsa.cpp:110-111,
// P2PeerConPipe.cpp:79-80, P2PeerCon232.cpp:102-103):
//
//     m_oThatP2Paddr   the connection's own identity  -> the routing key
//     m_oP2Padomain    a P2Padomain PATTERN           -> the accept filter
//
// The second one is the interesting half: P2Padomain::IsMapped does glob
// matching ('*', '?', '#' = digit) over '|'-separated alternatives, and a
// bare "*" sets m_bWildcard and short-circuits permissive
// (P2Peer.cpp:450-491, 522-539, 580-609). So a listener may be armed with a
// PATTERN, and P2PeerCon::OnLogin adopts the name the dialer actually claims
// (P2PeerCon.cpp:1877-1890) as the connection's routing key.
//
// Proven here, in order:
//   W1  listen(L"*")      accepts several differently-named dialers on ONE
//                         listener, adopts each claimed name, routes both ways
//   W2  listen(L"Demo.*") is a real filter -- in-domain up, out-of-domain refused
//   W3  the same over a named pipe
//   W4  a pattern is an ordinary peer key: one per hub (duplicate rejected)
//   W6  the pattern matrix -- exactly which claimed names each pattern form
//                         admits, one listener + one dialer per case
//   W7  closing a hub that still owns live connections (regression: this
//                         used to corrupt the heap -- kernel fix in
//                         CloseP2PmsgHub, P2Pwin32.cpp)
//   W5  the NEGATIVE case -- an EMPTY peer is not a wildcard, and is now
//       REFUSED at the call site rather than accepted and left to fail in
//       silence. The empty string nulls m_oThatP2Paddr as well as the domain,
//       and OnLogin's mandatory identity check (P2PeerCon.cpp:1857-1862,
//       reading GetP2Paddress() == m_oThatP2Paddr) throws "Null local
//       P2PmsgHub address" before the permissive domain branch is ever
//       reached -- so every login would be refused and the dialer would retry
//       forever with nothing reported. E_INVALIDARG at arm time replaces that
//       whole failure mode, and W5 pins it.
//
// Includes ONLY the facade's public headers, like FacadeSmokeTest.
// Exit code 0 = PASS, 1 = FAIL.

#include "TargetFacadeFn.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny check harness (same shape as FacadeSmokeTest)
// ---------------------------------------------------------------------------
static int g_nFailed = 0;

static void Check ( bool bOk, const char *lpszWhat )
{
    std::printf ( "  [%s] %s\n", bOk ? "PASS" : "FAIL", lpszWhat );
    std::fflush ( stdout );
    if ( !bOk ) ++g_nFailed;
}

// "Armed", for the many pairs below whose topology is beside the point. A
// pattern listener is never classified, but the DIALERS here are siblings of
// the servers they call ("Star.A" -> "Star.Server"), which Connect arms and
// then reports as P2PF_S_UNRELATED_LINK -- a success code. What this test is
// about is which names a pattern admits, so both successes count as armed.
static bool Armed ( HRESULT hr )
{
    return hr == S_OK || hr == p2pf::P2PF_S_UNRELATED_LINK;
}

// Endpoint spellings, built once so the port stays next to the URI it is in.
static std::wstring TcpListen ( unsigned short port )
{
    return L"tcp://:" + std::to_wstring ( port );
}
static std::wstring TcpDial ( unsigned short port )
{
    return L"tcp://127.0.0.1:" + std::to_wstring ( port );
}

struct Flag
{
    std::atomic<bool> m_bSet{false};
    void Set ( ) { m_bSet = true; }
    bool Wait ( int nMillisecs )
    {
        for ( int i = 0; i < nMillisecs / 10; ++i )
        {
            if ( m_bSet ) return true;
            ::Sleep ( 10 );
        }
        return m_bSet;
    }
};

// Every peer name a hub reported up -- i.e. which name a pattern listener
// ended up keyed on.
struct PeerLog
{
    std::mutex                m_oMutex;
    std::vector<std::wstring> m_aUp;

    void Up ( const wchar_t *p )
    {
        std::lock_guard<std::mutex> g ( m_oMutex );
        m_aUp.push_back ( p ? p : L"<null>" );
    }
    bool Has ( const wchar_t *p )
    {
        std::lock_guard<std::mutex> g ( m_oMutex );
        for ( auto& s : m_aUp ) if ( s == p ) return true;
        return false;
    }
    void Dump ( const char *lpszWho )
    {
        std::lock_guard<std::mutex> g ( m_oMutex );
        for ( auto& s : m_aUp )
          std::wprintf ( L"       %hs OnPeerUp : [%s]\n", lpszWho, s.c_str() );
        std::fflush ( stdout );
    }
};

// One armed pattern listener, kept alive for the whole matrix.
//
// A listener must NOT be closed while its refused dialer is still redialling:
// the listener throws out of OnLogin on every refused login, and closing it in
// that state kills the process (measured -- 0xC0000409 in Release; a 1.5 s
// settle only moved the crash one case later). W2 shows the safe shape: drop
// the dialer, and close the listener much later, once nothing is arriving.
// So the matrix arms each pattern once and only ever creates/destroys dialers.
struct PatternListener
{
    p2pf::Hub         hub;
    std::wstring      name;
    std::wstring      adopted;              // written on the pump thread
    std::atomic<bool> up{false};
};

static std::vector<std::unique_ptr<PatternListener>> g_aPatternListeners;

static PatternListener& Arm ( p2pf::Network& net, const wchar_t *pattern
                            , const wchar_t *srvName, unsigned short port )
{
    auto pOwned = std::make_unique<PatternListener>();
    PatternListener& oL = *pOwned;
    oL.name = srvName;
    oL.hub  = net.createHub ( srvName );
    oL.hub.onPeerUp ( [&oL](const wchar_t *p)
                      { oL.adopted = p ? p : L""; oL.up = true; } );
    HRESULT hr = oL.hub.listen ( pattern, TcpListen ( port ).c_str() );
    if ( FAILED(hr) )
    {
      std::wprintf ( L"  [FAIL] listen(L\"%s\") -> 0x%08lX\n", pattern, (unsigned long)hr );
      ++g_nFailed;
    }
    g_aPatternListeners.push_back ( std::move(pOwned) );
    return oL;
}

// Dial an armed pattern listener from a hub named `claim` and report whether
// the login completed -- and, when it did, that the CLAIMED name (not the
// pattern) is what the connection ended up keyed on. A refusal is invisible by
// construction, so the negative cases can only be established by waiting out
// the redial loop.
static void Try ( p2pf::Network& net, PatternListener& oL, const wchar_t *pattern
                , const wchar_t *claim, unsigned short port, bool bExpect )
{
    oL.up = false;
    oL.adopted.clear();

    Flag oCliUp;
    p2pf::Hub cli = net.createHub ( claim );
    cli.onPeerUp ( [&](const wchar_t*){ oCliUp.Set(); } );
    cli.connect ( oL.name.c_str(), TcpDial ( port ).c_str() );

    bool bUp = oCliUp.Wait ( 6000 );
    for ( int i = 0; bUp && !oL.up && i < 50; ++i ) ::Sleep ( 10 );

    bool bAdopted = ( oL.adopted == claim );
    std::wprintf ( L"  [%s] listen(L\"%s\") <- hub named [%s] => %s%s\n",
                   ( bUp == bExpect && ( !bUp || bAdopted ) ) ? L"PASS" : L"FAIL",
                   pattern, claim,
                   bUp ? L"ACCEPTED" : L"REFUSED",
                   ( bUp && !bAdopted ) ? L"  ADOPTED THE WRONG NAME" : L"" );
    std::fflush ( stdout );
    if ( bUp != bExpect || ( bUp && !bAdopted ) ) ++g_nFailed;

    cli.close();        // the dialer is always safe to drop
}

int main ( )
{
    std::printf ( "=== TargetFacade wildcard-listener test ===\n\n" );

    p2pf::Network net;

    // -----------------------------------------------------------------
    // W1. listen(L"*") -- one listener, two differently-named dialers
    // -----------------------------------------------------------------
    std::printf ( "-- W1. listen(L\"*\", L\"tcp://:7771\"), two dialers --\n" );

    PeerLog oSrvLog;
    Flag oSrvUpA, oSrvUpB, oUpA, oUpB, oEchoA, oEchoB, oUpstream;
    std::wstring strEchoA, strEchoB, strUpstream;

    p2pf::Hub server = net.createHub ( L"Star.Server" );
    server.onPeerUp ( [&](const wchar_t *p)
                      {
                        oSrvLog.Up ( p );
                        if ( std::wstring(p ? p : L"") == L"Star.A" ) oSrvUpA.Set();
                        else                                         oSrvUpB.Set();
                      } );
    server.onTopic  ( L"up", [&](const p2pf::Message& m)
                      { strUpstream = m.text() ? m.text() : L""; oUpstream.Set(); } );

    Check ( server.listen ( L"*", L"tcp://:7771" ) == S_OK,
            "W1.1 Listen accepted a bare '*' peer pattern" );

    p2pf::Hub a = net.createHub ( L"Star.A" );
    a.onPeerUp ( [&](const wchar_t*){ oUpA.Set(); } );
    a.onTopic  ( L"echo", [&](const p2pf::Message& m)
                 { strEchoA = m.text() ? m.text() : L""; oEchoA.Set(); } );
    Check ( Armed ( a.connect ( L"Star.Server", L"tcp://127.0.0.1:7771" ) ),
            "W1.2 dialer Star.A armed" );
    Check ( oUpA.Wait    ( 15000 ), "W1.3 dialer Star.A saw OnPeerUp" );
    Check ( oSrvUpA.Wait ( 15000 ), "W1.4 '*' listener saw OnPeerUp for Star.A" );

    p2pf::Hub b = net.createHub ( L"Star.B" );
    b.onPeerUp ( [&](const wchar_t*){ oUpB.Set(); } );
    b.onTopic  ( L"echo", [&](const p2pf::Message& m)
                 { strEchoB = m.text() ? m.text() : L""; oEchoB.Set(); } );
    Check ( Armed ( b.connect ( L"Star.Server", L"tcp://127.0.0.1:7771" ) ),
            "W1.5 dialer Star.B armed onto the SAME listener" );
    Check ( oUpB.Wait    ( 15000 ), "W1.6 dialer Star.B saw OnPeerUp" );
    Check ( oSrvUpB.Wait ( 15000 ), "W1.7 '*' listener saw OnPeerUp for Star.B" );

    oSrvLog.Dump ( "server" );
    Check ( oSrvLog.Has ( L"Star.A" ) && oSrvLog.Has ( L"Star.B" ),
            "W1.8 listener adopted BOTH claimed names" );
    Check ( server.isPeerUp ( L"Star.A" ) && server.isPeerUp ( L"Star.B" ),
            "W1.9 IsPeerUp true for both adopted names" );

    // The adopted name is a real routing key, not just an event label.
    Check ( server.sendText ( L"Star.A", L"echo", L"to A" ) == S_OK &&
            server.sendText ( L"Star.B", L"echo", L"to B" ) == S_OK,
            "W1.10 sendText to both adopted names returned S_OK" );
    Check ( oEchoA.Wait ( 5000 ) && strEchoA == L"to A", "W1.11 Star.A received its message" );
    Check ( oEchoB.Wait ( 5000 ) && strEchoB == L"to B", "W1.12 Star.B received its message" );
    Check ( a.sendText ( L"Star.Server", L"up", L"from A" ) == S_OK &&
            oUpstream.Wait ( 5000 ) && strUpstream == L"from A",
            "W1.13 dialer -> pattern listener routing works too" );

    // -----------------------------------------------------------------
    // W2. listen(L"Demo.*") -- a genuine filter, not a free-for-all
    // -----------------------------------------------------------------
    std::printf ( "\n-- W2. listen(L\"Demo.*\", L\"tcp://:7772\"): Demo.Alpha in, Other.Beta out --\n" );

    Flag oDomUp, oAlphaUp, oBetaUp;

    p2pf::Hub dom = net.createHub ( L"Dom.Server" );
    dom.onPeerUp ( [&](const wchar_t *p)
                   { if ( std::wstring(p ? p : L"") == L"Demo.Alpha" ) oDomUp.Set(); } );
    Check ( dom.listen ( L"Demo.*", L"tcp://:7772" ) == S_OK,
            "W2.1 Listen accepted a 'Demo.*' peer pattern" );

    p2pf::Hub alpha = net.createHub ( L"Demo.Alpha" );
    alpha.onPeerUp ( [&](const wchar_t*){ oAlphaUp.Set(); } );
    Check ( Armed ( alpha.connect ( L"Dom.Server", L"tcp://127.0.0.1:7772" ) ), "W2.2 Demo.Alpha armed" );
    Check ( oAlphaUp.Wait ( 15000 ), "W2.3 Demo.Alpha (in domain) came up" );
    Check ( oDomUp.Wait   ( 15000 ), "W2.4 listener keyed the connection on Demo.Alpha" );

    p2pf::Hub beta = net.createHub ( L"Other.Beta" );
    beta.onPeerUp ( [&](const wchar_t*){ oBetaUp.Set(); } );
    Check ( Armed ( beta.connect ( L"Dom.Server", L"tcp://127.0.0.1:7772" ) ), "W2.5 Other.Beta armed" );
    Check ( !oBetaUp.Wait ( 6000 ),            "W2.6 Other.Beta (out of domain) was REFUSED" );
    Check ( !dom.isPeerUp ( L"Other.Beta" ),   "W2.7 listener never marked Other.Beta up" );
    beta.close();       // stop its redial loop before it floods the log

    // -----------------------------------------------------------------
    // W3. same thing over a named pipe
    // -----------------------------------------------------------------
    std::printf ( "\n-- W3. listen(L\"*\", pipe://...) --\n" );

    PeerLog oPipeLog;
    Flag oPipeSrvUp, oPipeCliUp;
    const wchar_t *kPipe = L"pipe://\\\\.\\pipe\\p2pf_wildcard";

    p2pf::Hub pipeSrv = net.createHub ( L"StarPipe.Server" );
    pipeSrv.onPeerUp ( [&](const wchar_t *p){ oPipeLog.Up(p); oPipeSrvUp.Set(); } );
    Check ( pipeSrv.listen ( L"*", kPipe ) == S_OK, "W3.1 a pipe listener accepted '*'" );

    p2pf::Hub pipeCli = net.createHub ( L"StarPipe.Client" );
    pipeCli.onPeerUp ( [&](const wchar_t*){ oPipeCliUp.Set(); } );
    Check ( Armed ( pipeCli.connect ( L"StarPipe.Server", kPipe ) ), "W3.2 pipe dial armed" );
    Check ( oPipeCliUp.Wait ( 15000 ), "W3.3 pipe dialer came up" );
    Check ( oPipeSrvUp.Wait ( 15000 ), "W3.4 '*' pipe listener came up" );
    oPipeLog.Dump ( "pipeSrv" );
    Check ( oPipeLog.Has ( L"StarPipe.Client" ),
            "W3.5 pipe listener adopted the claimed name" );

    // -----------------------------------------------------------------
    // W4. a pattern is still an ordinary peer key -- one '*' per hub
    // -----------------------------------------------------------------
    std::printf ( "\n-- W4. duplicate-key behaviour of a pattern --\n" );
    Check ( server.listen ( L"*", L"tcp://:7773" ) == p2pf::P2PF_E_CON_DUPLICATE,
            "W4.1 a second '*' listener on the same hub is a duplicate peer" );

    // -----------------------------------------------------------------
    // W6. the pattern matrix -- exactly what each form admits
    // -----------------------------------------------------------------
    std::printf ( "\n-- W6. pattern matrix (each refusal costs a 6 s redial window) --\n" );

    // Every name here is in a private "Mx" namespace: two live hubs sharing
    // one address inside a single process corrupts the kernel's hub registry
    // (measured -- it took a Release crash to find), so no probe name may
    // collide with a hub any other section still has open.
    //
    // Accepting cases run before refusing ones on each listener, so an
    // accept is never asked of a listener that has already thrown.
    const wchar_t *kStar = L"*";
    const wchar_t *kGlob = L"MxD.*";
    const wchar_t *kAlt  = L"MxP.A|MxP.B";
    const wchar_t *kHash = L"MxN.#";
    const wchar_t *kQues = L"MxQ.?";
    const wchar_t *kSet  = L"MxS.<A,B>";

    PatternListener& oStar = Arm ( net, kStar, L"MxSrv.Star", 7801 );
    Try ( net, oStar, kStar, L"MxAny.At.All", 7801, true  );  // special-cased, no matching at all

    PatternListener& oGlob = Arm ( net, kGlob, L"MxSrv.Glob", 7802 );
    Try ( net, oGlob, kGlob, L"MxD.Alpha",    7802, true  );
    Try ( net, oGlob, kGlob, L"MxD.A.B.C",    7802, true  );  // '*' spans dots
    Try ( net, oGlob, kGlob, L"MxD",          7802, false );  // the stem alone is NOT in MxD.*
    Try ( net, oGlob, kGlob, L"MxOther.A",    7802, false );
    Try ( net, oGlob, kGlob, L"mxd.Alpha",    7802, false );  // matching is CASE-SENSITIVE

    PatternListener& oAlt = Arm ( net, kAlt, L"MxSrv.Alt", 7803 );
    Try ( net, oAlt, kAlt, L"MxP.A",          7803, true  );
    Try ( net, oAlt, kAlt, L"MxP.B",          7803, true  );
    Try ( net, oAlt, kAlt, L"MxP.C",          7803, false );

    PatternListener& oHash = Arm ( net, kHash, L"MxSrv.Hash", 7804 );
    Try ( net, oHash, kHash, L"MxN.42",       7804, true  );  // '#' = a run of digits
    Try ( net, oHash, kHash, L"MxN.x",        7804, false );

    PatternListener& oQues = Arm ( net, kQues, L"MxSrv.Ques", 7805 );
    Try ( net, oQues, kQues, L"MxQ.x",        7805, true  );  // '?' = one non-'.' character
    Try ( net, oQues, kQues, L"MxQ.xy",       7805, false );

    PatternListener& oSet = Arm ( net, kSet, L"MxSrv.Set", 7806 );
    Try ( net, oSet, kSet, L"MxS.A",          7806, true  );  // set expansion, P2Peer.cpp:611-725
    Try ( net, oSet, kSet, L"MxS.C",          7806, false );

    // -----------------------------------------------------------------
    // W7. closing a hub that still owns LIVE connections
    //
    // This used to corrupt the heap and kill the process, at a different
    // connection every run. RunHub answers P2PsigHub_CLOSE by posting
    // P2PsigCon_DESTROY to each con and then breaking out of the pump loop
    // (P2PeerHub.cpp:386-396), so nothing ever pumps those DESTROYs;
    // teardown falls to CloseP2PmsgHub's sweep, which walked
    // m_oCListP2PmsgCon with a POSITION while Drop(0) deleted entries out
    // of that same list. Fixed on both sides, and both are needed: the
    // kernel sweep now re-derives its position from an index each pass
    // (P2Pwin32.cpp, CloseP2PmsgHub), and FacadeHub::DrainCons retires the
    // connections first, while the pump can still process the request.
    //
    // The listener is closed FIRST here on purpose -- that is the order
    // that used to die.
    // -----------------------------------------------------------------
    std::printf ( "\n-- W7. close hubs that still own live connections --\n" );

    int nCycles = 0;
    for ( int i = 0; i < 8; ++i )
    {
      unsigned short port = (unsigned short)( 7820 + i );
      std::wstring   strSrv = L"Churn.Srv" + std::to_wstring ( port );
      std::wstring   strCli = L"Churn.Cli" + std::to_wstring ( port );

      Flag oUp;
      p2pf::Hub churnSrv = net.createHub ( strSrv.c_str() );
      churnSrv.listen ( L"Churn.*", TcpListen ( port ).c_str() );

      p2pf::Hub churnCli = net.createHub ( strCli.c_str() );
      churnCli.onPeerUp ( [&](const wchar_t*){ oUp.Set(); } );
      churnCli.connect ( strSrv.c_str(), TcpDial ( port ).c_str() );

      if ( !oUp.Wait ( 15000 ) )
        break;                       // never came up; not what we are testing

      churnSrv.close();              // <-- listener first, with the link live
      churnCli.close();
      ++nCycles;
    }
    Check ( nCycles == 8,
            "8 create/connect/close cycles, listener closed while linked" );

    // -----------------------------------------------------------------
    // W5. the negative: an EMPTY peer is NOT a wildcard
    // -----------------------------------------------------------------
    // This used to be the loudest section in the suite: an empty peer was
    // ACCEPTED, every login was then refused inside the kernel, and the dialer
    // redialled forever while P2Pevent dumps scrolled past. None of that is
    // reachable any more -- the guard turns the whole failure mode into one
    // synchronous E_INVALIDARG, which is why this section is now four quiet
    // checks instead of an 8-second wait for something never to happen.
    std::printf ( "\n-- W5. an EMPTY peer is refused at the call site --\n" );

    p2pf::Hub emptySrv = net.createHub ( L"Empty.Server" );
    Check ( emptySrv.listen  ( L"", L"tcp://:7774" ) == E_INVALIDARG,
            "W5.1 Listen refuses an empty peer" );
    Check ( emptySrv.connect ( L"", L"tcp://127.0.0.1:7774" ) == E_INVALIDARG,
            "W5.2 Connect refuses it too" );
    Check ( emptySrv.listen  ( nullptr, L"tcp://:7774" ) == E_POINTER,
            "W5.3 a NULL peer stays E_POINTER, distinct from empty" );

    unsigned int uEmptyCons = 0;
    Check ( emptySrv.raw()->GetConCount ( &uEmptyCons ) == S_OK && uEmptyCons == 0,
            "W5.4 a refused arm leaves the hub with no connection at all" );
    emptySrv.close();

    // -----------------------------------------------------------------
    std::printf ( "\n-- teardown --\n" );
    g_aPatternListeners.clear();        // safe now: every dialer is long gone
    pipeCli.close(); pipeSrv.close();
    alpha.close();   dom.close();
    b.close(); a.close(); server.close();
    Check ( true, "all hubs closed without hanging" );

    std::printf ( "\n=== %s (%d failure%s) ===\n",
                  g_nFailed ? "FAIL" : "PASS",
                  g_nFailed, g_nFailed == 1 ? "" : "s" );
    return g_nFailed ? 1 : 0;
}
