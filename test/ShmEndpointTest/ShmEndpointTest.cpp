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
// ShmEndpointTest.cpp
//
// shm:// through the facade (p2p_performance.md §6.3a S3; Targetcore 3.10.0).
//
// shm://NAME is pipe://NAME with the bytes moved: the NAME is a pipe name, taken
// verbatim, the pipe is the rendezvous and control channel, and the data runs through
// two rings in a shared mapping (P2PeerConShm / P2PeerioShm). The facade adds only the
// scheme, the canonical spelling, and a redialling client (RetryDialShm, as
// RetryDialPipe). Its own test rather than a section of FacadeSmokeTest, because that
// file is already at the process's sixteen-hub budget.
//
// Proven here, in order:
//   S1  the grammar: "shm://" with no name is refused, and an endpoint read back
//       through setEndpoint/endpointFor is spelt shm://NAME (no silent pipe://)
//   S2  listen + connect over shm:// come up, and a text round-trips
//   S3  traffic that laps the 64 KB ring many times over arrives complete and in
//       order (partial ring writes, wrap-around, the writer parking on a full ring)
//   S4  a dial ARMED BEFORE its listener comes up when the listener does (the
//       redial path: a failed pipe open makes the shm handshake a no-op)
//
// Includes ONLY the facade's public headers, like FacadeSmokeTest.
// Exit code 0 = PASS, 1 = FAIL.

#include "TargetFacadeFn.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

static int g_nFailed = 0;

static void Check ( bool bOk, const char *lpszWhat )
{
    std::printf ( "  [%s] %s\n", bOk ? "PASS" : "FAIL", lpszWhat );
    std::fflush ( stdout );
    if ( !bOk ) ++g_nFailed;
}

static bool Armed ( HRESULT hr )
{
    return hr == S_OK || hr == p2pf::P2PF_S_UNRELATED_LINK;
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

//  Message k of the S3 stream: its index, then a body long enough that a few dozen
//  of them lap the ring, varied so a dropped or reordered chunk cannot match
static std::wstring StreamBody ( int k )
{
    std::wstring s = std::to_wstring ( k ) + L":";
    const size_t n = 6000 + ( k * 397 ) % 5000;          // 12-22 KB of UTF-16
    for ( size_t i = 0; i < n; ++i )
      s.push_back ( (wchar_t)( L'A' + ( i + k ) % 26 ) );
    return s;
}

int main ( )
{
    std::printf ( "=== TargetFacade shm:// endpoint test ===\n\n" );

    p2pf::Network net;

    // -----------------------------------------------------------------
    // S1. the grammar
    // -----------------------------------------------------------------
    std::printf ( "-- S1. grammar --\n" );
    {
      p2pf::Hub g = net.createHub ( L"ShmGrammar" );
      Check ( g.listen ( L"ShmGrammar.X", L"shm://" ) == p2pf::P2PF_E_ENDPOINT,
              "S1.1 shm:// with no name is refused" );
      Check ( net.setEndpoint ( L"ShmCfg.Late", L"shm://P2PshmCfgLate" ) == S_OK &&
              net.endpointFor ( L"ShmCfg.Late" ) == L"shm://P2PshmCfgLate",
              "S1.2 an shm endpoint reads back as shm://NAME" );
      g.close();
    }

    // -----------------------------------------------------------------
    // S2. listen + connect, one round trip
    // -----------------------------------------------------------------
    std::printf ( "\n-- S2. listen + connect over shm:// --\n" );
    const wchar_t *kShm = L"shm://\\\\.\\pipe\\p2pf_shm_endpoint";

    Flag oUp, oPing;
    std::mutex oMx;
    std::vector<std::wstring> aGot;
    std::string strPing;

    p2pf::Hub srv = net.createHub ( L"Shm" );
    p2pf::Hub cli = net.createHub ( L"Shm.Node" );
    srv.onTopic ( L"ping", [&](const p2pf::Message& m)
                  {
                    const wchar_t *w = m.text();
                    while ( w && *w ) strPing += (char)*w++;
                    oPing.Set();
                  } );
    srv.onTopic ( L"stream", [&](const p2pf::Message& m)
                  {
                    std::lock_guard<std::mutex> g ( oMx );
                    aGot.push_back ( m.text() ? m.text() : L"" );
                  } );
    cli.onPeerUp ( [&](const wchar_t*){ oUp.Set(); } );

    const HRESULT hrListen = srv.listen ( L"Shm.Node", kShm );
    Check ( Armed ( hrListen ), "S2.1 shm service armed" );
    if ( !Armed ( hrListen ) )
      std::printf ( "       listen returned 0x%08lX\n", (unsigned long)hrListen );
    Check ( Armed ( cli.connect ( L"Shm", kShm ) ), "S2.2 shm client armed" );
    Check ( oUp.Wait ( 15000 ), "S2.3 shm peer came up" );
    Check ( cli.sendText ( L"Shm", L"ping", L"over shm" ) == S_OK,
            "S2.4 sendText returned S_OK" );
    Check ( oPing.Wait ( 5000 ) && strPing == "over shm", "S2.5 payload round-tripped" );

    // -----------------------------------------------------------------
    // S3. many laps of the ring
    // -----------------------------------------------------------------
    std::printf ( "\n-- S3. a stream that laps the 64 KB ring --\n" );
    const int kN = 120;                                    // ~2 MB through a 64 KB ring
    int nSent = 0;
    for ( int k = 0; k < kN; ++k )
      if ( cli.sendText ( L"Shm", L"stream", StreamBody ( k ).c_str() ) == S_OK ) ++nSent;
    Check ( nSent == kN, "S3.1 every send was accepted" );

    for ( int i = 0; i < 1500; ++i )
    {
      { std::lock_guard<std::mutex> g ( oMx ); if ( (int)aGot.size() >= kN ) break; }
      ::Sleep ( 10 );
    }
    bool bOrder = true;
    size_t nGot = 0;
    {
      std::lock_guard<std::mutex> g ( oMx );
      nGot = aGot.size();
      for ( size_t k = 0; k < nGot && bOrder; ++k )
        bOrder = aGot[k] == StreamBody ( (int)k );
    }
    std::printf ( "       %zu of %d messages arrived\n", nGot, kN );
    Check ( nGot == (size_t)kN, "S3.2 all of them arrived" );
    Check ( bOrder,             "S3.3 each one intact and in order" );

    // -----------------------------------------------------------------
    // S4. dial first, listen later
    // -----------------------------------------------------------------
    std::printf ( "\n-- S4. a dial armed before its listener --\n" );
    const wchar_t *kShmLate = L"shm://\\\\.\\pipe\\p2pf_shm_late";
    Flag oLateUp;
    p2pf::Hub lateSrv = net.createHub ( L"ShmLate" );
    p2pf::Hub lateCli = net.createHub ( L"ShmLate.Node" );
    lateCli.onPeerUp ( [&](const wchar_t*){ oLateUp.Set(); } );
    Check ( Armed ( lateCli.connect ( L"ShmLate", kShmLate ) ), "S4.1 dial armed with nobody listening" );
    ::Sleep ( 1500 );                                      // a few failed redials
    Check ( !oLateUp.m_bSet, "S4.2 not up while nobody listens" );
    Check ( Armed ( lateSrv.listen ( L"ShmLate.Node", kShmLate ) ), "S4.3 listener armed late" );
    Check ( oLateUp.Wait ( 15000 ), "S4.4 the redialling client came up" );

    // -----------------------------------------------------------------
    std::printf ( "\n-- teardown --\n" );
    lateCli.close(); lateSrv.close();
    cli.close();     srv.close();
    Check ( true, "all hubs closed without hanging" );

    std::printf ( "\n=== %s (%d failure%s) ===\n",
                  g_nFailed ? "FAIL" : "PASS",
                  g_nFailed, g_nFailed == 1 ? "" : "s" );
    return g_nFailed ? 1 : 0;
}
