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
// FacadeSmokeTest.cpp
//
// End-to-end proof that TargetFacade.dll works AND that it really isolates:
// this translation unit includes ONLY the facade's public headers -- no MFC,
// no afx*, no Targetcore, no WinSock. If the facade leaked its internals this
// file would not compile.
//
// Covered:
//   1  one init object brings the kernel up (no StartupP2Pmsg/WSAStartup here)
//   2  TCP loopback: Listen + Connect -> OnPeerUp on BOTH sides
//   3  unicast SendText -> per-topic std::function handler on the far side
//   4  binary Send      -> exact byte round-trip
//   5  Broadcast        -> arrives with broadcast=true under its own topic
//   6  IsPeerUp reflects the live handshake state
//   7  in-process Dmx transport (no socket, no OS handle)
//   8  error contract: reserved topic and duplicate peer address are rejected
//   9  clean teardown: hubs closed, kernel shut down, no hang
//  10  one verb pair for every transport, with the transport as an endpoint
//  11  the endpoint grammar, every rejection case, plus the peer guards
//  12  topology: sibling links armed but reported, patterns not classified
//  13  in-process resolution: no endpoint on either side, no configuration
//  14  IP2PNetwork::Link -- both ends of an edge, and its failure contract
//  15  the read side: GetConCount/GetCon/GetEndpoint/Describe, the routing
//      relation per peer, and the caller-sized buffer protocol
//  16  closing a hub that owns an armed-but-never-connected Dmx listener
//  17  a RESOLVED dial retries the in-process rendezvous; a TYPED one still
//      does not
//  18  the deployment map: an endpoint that appears nowhere in the code
//  19  a whole process -- hubs, endpoints, edges -- from one block of text
//  20  the login accept filter, the same on every transport
//  21  ABI 6: disconnect, the escape hatch, pump-thread timers, post-to-pump,
//      a message answer + coded events, con options and ping
//  22  the boundaries of those ten: a timer that never fires early, a Close()
//      that a blocked Ping cannot outlive, and a declined message reported to
//      the sender on every path
//  23  ABI 7: a correlation tag that survives a real TCP wire, request/response
//      by tag alone, priority, and what the destination actually reads
//  24  ABI 7: fire-and-forget, proved by the same decline with and without it
//  25  ABI 8: named fields beside the payload -- that the kernel's message
//      TREE crosses a wire, and that the payload is undisturbed by it
//  26  ABI 9: a hub that runs on the CALLER's thread -- it does nothing until
//      pumped, and every callback then arrives on the thread that pumped it
//  27  ABI 10: the kernel's own diagnostic stream -- that it arrives, that it
//      arrives SYNCHRONOUSLY on the raising thread, and that the mask, the
//      current-record rule and the feedback breaker all hold
//
// Exit code 0 = PASS, 1 = FAIL (each check prints its own line).

#include "TargetFacadeFn.hpp"        // pulls in TargetFacade.h
#include "TargetFacadeTopology.hpp"  // optional header-only C5 helper

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny check harness
// ---------------------------------------------------------------------------
static int g_nFailed = 0;

static void Check ( bool bOk, const char *lpszWhat )
{
    std::printf ( "  [%s] %s\n", bOk ? "PASS" : "FAIL", lpszWhat );
    std::fflush ( stdout );
    if ( !bOk ) ++g_nFailed;
}

// "The connection was armed", for a pair whose topology is not the point of
// the check. Listen/Connect classify every pair, and the classic demo naming
// -- "Demo.Server" beside "Demo.Client" -- is a SIBLING pair, so those calls
// legitimately answer P2PF_S_UNRELATED_LINK rather than S_OK. Both mean armed;
// section 12 is where the distinction itself is pinned.
static bool Armed ( HRESULT hr )
{
    return hr == S_OK || hr == p2pf::P2PF_S_UNRELATED_LINK;
}

// Section 16 rounds. The failure it hunts needs the ALLOCATOR to hand a fresh
// connection the address of a freed one, so a single round would make the test
// a coin toss. Measured pre-fix: the abort landed on round 3.
static const int kDrainRounds = 8;

// Section 17 rounds.
static const int kRaceRounds = 6;

// Park the caller a hair before the next whole second.
//
// This is the forcing function for section 17's first check, and it exists
// because the kernel's Dmx rendezvous budget is computed as
// `_time64(0)*1000 + 100` against a ONE-SECOND-GRANULARITY clock
// (P2PeerConDmx.cpp:682, 727-729). The spin therefore lasts until the next
// second boundary, whenever that happens to be: a dial that starts 5 ms before
// one gets 5 ms to find its listener, and a dial that starts just after one
// gets a full second. Arming right before a boundary is the difference between
// a race this suite loses now and then and a race it loses nearly every time.
//
// FILETIME rather than the CRT clock because the two share second boundaries
// (their epochs differ by a whole number of seconds), and this needs the
// position WITHIN the second that _time64 is about to read.
static void ParkBeforeSecondBoundary ( int nMillisecsBefore )
{
    for ( ;; )
    {
      FILETIME ft;
      ::GetSystemTimeAsFileTime ( &ft );
      unsigned long long uTicks = ( (unsigned long long)ft.dwHighDateTime << 32 )
                                |   (unsigned long long)ft.dwLowDateTime;
      long nInto = (long)( ( uTicks % 10000000ULL ) / 10000ULL );  // ms
      long nLeft = 1000 - nInto;

      if ( nLeft <= nMillisecsBefore ) return;
      if ( nLeft >  30 ) ::Sleep ( (DWORD)( nLeft - 30 ) );
      else               ::Sleep ( 0 );
    }
}

// A callback-signalled flag with a bounded wait (callbacks land on pump
// threads, so the main thread must never spin forever).
struct Flag
{
    std::atomic<bool> m_bSet{false};

    void Set ( )                { m_bSet = true; }
    void Reset ( )              { m_bSet = false; }
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

int main ( )
{
    std::printf ( "=== TargetFacade smoke test ===\n" );

    // -----------------------------------------------------------------
    // 1. One init object. No StartupP2Pmsg, no WSAStartup, no SpawnHub.
    // -----------------------------------------------------------------
    p2pf::Network net;
    std::printf ( "version: %ls\n\n", net.raw()->VersionString() );

    // -----------------------------------------------------------------
    // 2. TCP loopback between two hubs
    // -----------------------------------------------------------------
    std::printf ( "-- TCP loopback --\n" );

    Flag        oServerUp, oClientUp, oChat, oBlob, oNews;
    std::string strChat;
    std::vector<unsigned char> aBlob;
    std::wstring strNewsTopic;
    bool        bNewsWasBroadcast = false;

    p2pf::Hub server = net.createHub ( L"Demo.Server" );
    server.onPeerUp   ( [&](const wchar_t*){ oServerUp.Set(); } );
    server.onTopic    ( L"chat", [&](const p2pf::Message& m)
                        {
                          const wchar_t *w = m.text();
                          while ( w && *w ) strChat += (char)*w++;
                          oChat.Set();
                        } );
    server.onTopic    ( L"blob", [&](const p2pf::Message& m)
                        {
                          const unsigned char *p = (const unsigned char*)m.payload;
                          aBlob.assign ( p, p + m.size );
                          oBlob.Set();
                        } );

    Check ( Armed ( server.listen ( L"Demo.Client", L"tcp://:7788" ) ),
            "server armed a TCP listener" );

    p2pf::Hub client = net.createHub ( L"Demo.Client" );
    client.onPeerUp   ( [&](const wchar_t*){ oClientUp.Set(); } );
    client.onMessage  ( [&](const p2pf::Message& m)
                        {
                          strNewsTopic      = m.topic;
                          bNewsWasBroadcast = m.broadcast;
                          oNews.Set();
                        } );

    Check ( Armed ( client.connect ( L"Demo.Server", L"tcp://127.0.0.1:7788" ) ),
            "client armed a TCP dial" );

    Check ( oClientUp.Wait ( 10000 ), "client saw OnPeerUp (login ack)" );
    Check ( oServerUp.Wait ( 10000 ), "server saw OnPeerUp (login)" );
    Check ( client.isPeerUp ( L"Demo.Server" ), "client IsPeerUp(server)" );
    Check ( server.isPeerUp ( L"Demo.Client" ), "server IsPeerUp(client)" );

    // -----------------------------------------------------------------
    // 3. Unicast text under a topic
    // -----------------------------------------------------------------
    Check ( client.sendText ( L"Demo.Server", L"chat", L"hello facade" ) == S_OK,
            "client sendText returned S_OK" );
    Check ( oChat.Wait ( 5000 ), "server 'chat' handler fired" );
    Check ( strChat == "hello facade", "chat payload round-tripped intact" );

    // -----------------------------------------------------------------
    // 4. Binary payload, exact bytes
    // -----------------------------------------------------------------
    unsigned char aSent[256];
    for ( int i = 0; i < 256; ++i ) aSent[i] = (unsigned char)i;
    Check ( client.send ( L"Demo.Server", L"blob", aSent, sizeof(aSent) ) == S_OK,
            "client sent a 256-byte binary payload" );
    Check ( oBlob.Wait ( 5000 ), "server 'blob' handler fired" );
    Check ( aBlob.size() == sizeof(aSent) &&
            std::equal ( aBlob.begin(), aBlob.end(), aSent ),
            "binary payload round-tripped byte-for-byte" );

    // -----------------------------------------------------------------
    // 5. Broadcast -- topic preserved, flagged as a broadcast
    // -----------------------------------------------------------------
    Check ( server.broadcast ( L"news", "extra", 5 ) == S_OK,
            "server broadcast returned S_OK" );
    Check ( oNews.Wait ( 5000 ), "client received the broadcast" );
    Check ( strNewsTopic == L"news",  "broadcast kept its topic" );
    Check ( bNewsWasBroadcast,        "broadcast flagged broadcast=true" );

    // -----------------------------------------------------------------
    // 8. Error contract
    // -----------------------------------------------------------------
    std::printf ( "\n-- error contract --\n" );
    Check ( client.sendText ( L"Demo.Server", L"P2PmsgBCast", L"x" )
              == p2pf::P2PF_E_RESERVED_TOPIC,
            "reserved 'P2Pmsg*' topic rejected" );
    Check ( server.listen ( L"Demo.Client", L"tcp://:7799" )
              == p2pf::P2PF_E_CON_DUPLICATE,
            "duplicate peer address rejected" );

    // ...and the refusal must not have disturbed the connection that was
    // already there.  The first arm is still live and still routing; a rollback
    // that erased the record wholesale would report this peer as one the hub
    // never armed -- no endpoint, no role -- which is a different fact and a
    // false one.  The read side is what makes that observable at all.
    Check ( server.endpointFor ( L"Demo.Client" ) == L"tcp://:7788",
            "a refused duplicate leaves the original arm's endpoint intact" );

    // A second live hub on one address corrupts the kernel's hub registry
    // (unrelated logins start failing, then the process dies), so the facade
    // refuses it. Raw CreateHub here, not p2pf::Network::createHub, because
    // the convenience wrapper turns a failed create into an exception.
    {
      p2pf::HubEventsBase  oSink;
      p2pf::IP2PHub       *pDup = (p2pf::IP2PHub*)(void*)1;   // must be zeroed
      HRESULT hr = net.raw()->CreateHub ( L"Demo.Server", &oSink, &pDup );
      Check ( hr == p2pf::P2PF_E_HUB_DUPLICATE,
              "duplicate HUB address rejected" );
      Check ( pDup == nullptr, "a rejected CreateHub hands back no hub" );
      Check ( server.isPeerUp ( L"Demo.Client" ),
              "the existing hub of that name is untouched" );
    }

    // Closing a hub must free its address again -- otherwise the check above
    // would leak names for the life of the process.
    {
      p2pf::HubEventsBase  oSink;
      p2pf::IP2PHub       *pFirst = nullptr, *pAgain = nullptr;
      Check ( net.raw()->CreateHub ( L"Demo.Recycled", &oSink, &pFirst ) == S_OK,
              "created a throwaway hub" );
      if ( pFirst ) pFirst->Close();
      Check ( net.raw()->CreateHub ( L"Demo.Recycled", &oSink, &pAgain ) == S_OK,
              "closing a hub frees its address for reuse" );
      if ( pAgain ) pAgain->Close();
    }

    // -----------------------------------------------------------------
    // 7. In-process Dmx transport (same API, no socket)
    // -----------------------------------------------------------------
    std::printf ( "\n-- in-process Dmx --\n" );
    Flag oDmxUp, oDmxMsg;
    std::string strDmx;

    p2pf::Hub dmxA = net.createHub ( L"Dmx.A" );
    p2pf::Hub dmxB = net.createHub ( L"Dmx.B" );
    dmxA.onTopic ( L"ping", [&](const p2pf::Message& m)
                   {
                     const wchar_t *w = m.text();
                     while ( w && *w ) strDmx += (char)*w++;
                     oDmxMsg.Set();
                   } );
    dmxB.onPeerUp ( [&](const wchar_t*){ oDmxUp.Set(); } );

    Check ( Armed ( dmxA.listen  ( L"Dmx.B", L"dmx://facade.smoke.svc" ) ),
            "Dmx service armed" );
    Check ( Armed ( dmxB.connect ( L"Dmx.A", L"dmx://facade.smoke.svc" ) ),
            "Dmx client armed" );
    Check ( oDmxUp.Wait ( 10000 ), "Dmx peer came up" );
    Check ( dmxB.sendText ( L"Dmx.A", L"ping", L"over dmx" ) == S_OK,
            "Dmx sendText returned S_OK" );
    Check ( oDmxMsg.Wait ( 5000 ) && strDmx == "over dmx",
            "Dmx payload round-tripped" );

    // -----------------------------------------------------------------
    // 7b. Named-pipe transport (same machine, cross process capable)
    // -----------------------------------------------------------------
    std::printf ( "\n-- named pipe --\n" );
    Flag oPipeUp, oPipeMsg;
    std::string strPipe;
    // The scheme is the parser's only requirement; everything after it is the
    // pipe name VERBATIM, so a full \\.\pipe\name path goes through untouched.
    const wchar_t *kPipe = L"pipe://\\\\.\\pipe\\p2pf_smoke";

    p2pf::Hub pipeA = net.createHub ( L"Pipe.A" );
    p2pf::Hub pipeB = net.createHub ( L"Pipe.B" );
    pipeA.onTopic ( L"ping", [&](const p2pf::Message& m)
                    {
                      const wchar_t *w = m.text();
                      while ( w && *w ) strPipe += (char)*w++;
                      oPipeMsg.Set();
                    } );
    pipeB.onPeerUp ( [&](const wchar_t*){ oPipeUp.Set(); } );

    Check ( Armed ( pipeA.listen  ( L"Pipe.B", kPipe ) ),
            "pipe service armed" );
    Check ( Armed ( pipeB.connect ( L"Pipe.A", kPipe ) ),
            "pipe client armed" );
    Check ( oPipeUp.Wait ( 10000 ), "pipe peer came up" );
    Check ( pipeB.sendText ( L"Pipe.A", L"ping", L"over pipe" ) == S_OK,
            "pipe sendText returned S_OK" );
    Check ( oPipeMsg.Wait ( 5000 ) && strPipe == "over pipe",
            "pipe payload round-tripped" );

    // -----------------------------------------------------------------
    // 10. The endpoint verbs on a properly shaped (parent/child) pair
    // -----------------------------------------------------------------
    std::printf ( "\n-- unified endpoint verbs --\n" );
    Flag oUniUp, oUniMsg, oUniErr;
    std::string strUni;

    // Parent/child, not siblings: the arming verbs classify the pair and a
    // sibling link is a real (reported) topology problem -- see 12 below.
    p2pf::Hub uniA = net.createHub ( L"Uni" );
    p2pf::Hub uniB = net.createHub ( L"Uni.Node" );
    uniA.onTopic  ( L"ping", [&](const p2pf::Message& m)
                    {
                      const wchar_t *w = m.text();
                      while ( w && *w ) strUni += (char)*w++;
                      oUniMsg.Set();
                    } );
    uniB.onPeerUp ( [&](const wchar_t*){ oUniUp.Set(); } );
    uniA.onError  ( [&](const wchar_t*){ oUniErr.Set(); } );   // used by 12

    Check ( uniA.listen  ( L"Uni.Node", L"tcp://:7801" ) == S_OK,
            "Listen armed tcp://:7801" );
    Check ( uniB.connect ( L"Uni", L"tcp://127.0.0.1:7801" ) == S_OK,
            "Connect armed tcp://127.0.0.1:7801" );
    Check ( oUniUp.Wait ( 10000 ), "endpoint-armed TCP peer came up" );
    Check ( uniB.sendText ( L"Uni", L"ping", L"over uri" ) == S_OK,
            "sendText over an endpoint-armed link returned S_OK" );
    Check ( oUniMsg.Wait ( 5000 ) && strUni == "over uri",
            "endpoint-armed payload round-tripped" );

    // -----------------------------------------------------------------
    // 11. The grammar says no, at the call site, before anything is armed
    // -----------------------------------------------------------------
    // Reuses `uniA` rather than creating a hub per section. Two reasons, both
    // load-bearing: the facade starts the kernel with StartupP2Pmsg(16), so
    // sixteen live hubs is the budget for the whole process; and hubs are
    // closed together at the end rather than section by section, because
    // closing a hub that owns an armed-but-never-connected Dmx listener --
    // which several checks below deliberately create -- is not something the
    // facade currently does safely (see the teardown note at the end).
    std::printf ( "\n-- endpoint grammar --\n" );

    struct { const wchar_t *ep; const char *what; } aBad[] =
    {
      { L"7788",                   "no scheme"                         },
      { L"udp://host:1",           "unknown scheme"                    },
      { L"tcp://127.0.0.1",        "tcp with no port"                  },
      { L"tcp://:0",               "tcp port 0"                        },
      { L"tcp://:65536",           "tcp port out of range"             },
      { L"tcp://:port",            "tcp port not numeric"              },
      { L"tcp://[::1]:7788",       "IPv6 rejected, not mangled"        },
      { L"tcp://10.0.0.7:7788",    "listen may not bind one host"      },
      { L"pipe://",                "empty pipe name"                   },
      { L"dmx://",                 "empty dmx service"                 },
      { L"serial://COM0",          "COM0 out of range"                 },
      { L"serial://COM999",        "COM999 out of range"               },
      { L"serial://ttyS0",         "non-numeric COM port"              },
    };
    for ( size_t i = 0; i < sizeof(aBad)/sizeof(aBad[0]); ++i )
      Check ( uniA.listen ( L"Uni.Reject", aBad[i].ep ) == p2pf::P2PF_E_ENDPOINT,
              aBad[i].what );

    // A dial DOES need a host; a listen must not have one. Same string, two
    // answers -- which is the point of telling the parser the role.
    Check ( uniA.connect ( L"Uni.Reject", L"tcp://:7788" ) == p2pf::P2PF_E_ENDPOINT,
            "a dial with no host is rejected" );

    // The swapped-argument call: both parameters are const wchar_t*, so
    // nothing but this guard catches it. Left alone it arms a connection to a
    // peer literally addressed "tcp://:7788" and every message to it dies
    // silently as undeliverable.
    Check ( uniA.listen ( L"tcp://:7788", L"Uni.Reject" ) == E_INVALIDARG,
            "peer and endpoint swapped is rejected" );
    Check ( uniA.listen ( L"", L"tcp://:7788" ) == E_INVALIDARG,
            "empty peer is rejected (it can never log in)" );
    Check ( uniA.listen ( L"Uni", L"tcp://:7788" ) == E_INVALIDARG,
            "a hub may not link to itself" );
    Check ( uniA.listen ( L"Uni.Cap", L"TCP://:7802" ) == S_OK,
            "scheme is case-insensitive" );
    Check ( uniA.listen ( L"Uni.Svc", L"dmx:svc" ) == S_OK,
            "scheme: without // is accepted" );

    // An EMPTY endpoint is not a malformed one -- it is the request to
    // resolve, exactly like nullptr. It only reaches the grammar if it has
    // something in it.
    Check ( uniA.listen  ( L"Uni.Auto", L"" ) == S_OK,
            "an empty endpoint means 'resolve', same as nullptr" );
    Check ( uniA.connect ( L"Uni.Absent", L"" ) == p2pf::P2PF_E_UNRESOLVED,
            "...and resolving a dial with no sibling still fails loudly" );

    // -----------------------------------------------------------------
    // 12. Topology: the check the kernel never makes
    // -----------------------------------------------------------------
    std::printf ( "\n-- topology --\n" );
    // "Uni" and "Other.Place" are in different trees. Direct traffic between
    // them works; the edge can never be a transit hop -- so it is armed AND
    // reported, with a success code the caller can test for.
    Check ( uniA.listen ( L"Other.Place", L"tcp://:7803" )
              == p2pf::P2PF_S_UNRELATED_LINK,
            "unrelated link armed, flagged P2PF_S_UNRELATED_LINK" );
    Check ( SUCCEEDED ( p2pf::P2PF_S_UNRELATED_LINK ),
            "P2PF_S_UNRELATED_LINK is a success code, not an error" );
    Check ( oUniErr.Wait ( 2000 ), "unrelated link also raised one OnError" );
    Check ( uniA.listen ( L"Uni.Deep.Child", L"tcp://:7804" ) == S_OK,
            "a skip-level descendant is a normal link" );
    Check ( uniA.listen ( L"*", L"tcp://:7805" ) == S_OK,
            "a wildcard peer is not classified" );

    // -----------------------------------------------------------------
    // 13. In-process resolution: the address really is the whole handle
    // -----------------------------------------------------------------
    std::printf ( "\n-- resolved endpoints --\n" );
    Flag oAutoUp, oAutoMsg;
    std::string strAuto;

    p2pf::Hub autoA = net.createHub ( L"Auto" );
    p2pf::Hub autoB = net.createHub ( L"Auto.Sib" );
    autoA.onTopic  ( L"ping", [&](const p2pf::Message& m)
                     {
                       const wchar_t *w = m.text();
                       while ( w && *w ) strAuto += (char)*w++;
                       oAutoMsg.Set();
                     } );
    autoB.onPeerUp ( [&](const wchar_t*){ oAutoUp.Set(); } );

    // No endpoint on either side, and no agreement between them beyond the
    // two addresses they already knew.
    Check ( autoA.listen  ( L"Auto.Sib", nullptr ) == S_OK,
            "endpoint-less Listen resolved (derived in-process service)" );
    Check ( autoB.connect ( L"Auto", nullptr ) == S_OK,
            "endpoint-less Connect resolved against the sibling hub" );
    Check ( oAutoUp.Wait ( 10000 ), "resolved peer came up" );
    Check ( autoB.sendText ( L"Auto", L"ping", L"no endpoint" ) == S_OK,
            "sendText over a resolved link returned S_OK" );
    Check ( oAutoMsg.Wait ( 5000 ) && strAuto == "no endpoint",
            "resolved payload round-tripped" );

    // Resolution never guesses: it fails at the caller that made the mistake
    // rather than arming something that quietly never connects.
    Check ( autoB.connect ( L"Nobody.Here", nullptr ) == p2pf::P2PF_E_UNRESOLVED,
            "no such hub in this process -> P2PF_E_UNRESOLVED" );
    {
      p2pf::Hub lone = net.createHub ( L"Lonely" );
      Check ( autoB.connect ( L"Lonely", nullptr ) == p2pf::P2PF_E_UNRESOLVED,
              "hub present but not expecting us -> P2PF_E_UNRESOLVED" );
      lone.close();
    }

    // -----------------------------------------------------------------
    // 14. Link: both ends of an edge, in the order that works
    // -----------------------------------------------------------------
    std::printf ( "\n-- Link --\n" );
    Flag oLinkUp, oLinkMsg;
    std::string strLink;

    p2pf::Hub lkA = net.createHub ( L"Lk" );
    p2pf::Hub lkB = net.createHub ( L"Lk.Node" );
    lkA.onTopic  ( L"ping", [&](const p2pf::Message& m)
                   {
                     const wchar_t *w = m.text();
                     while ( w && *w ) strLink += (char)*w++;
                     oLinkMsg.Set();
                   } );
    lkB.onPeerUp ( [&](const wchar_t*){ oLinkUp.Set(); } );

    Check ( net.link ( L"Lk", L"Lk.Node" ) == S_OK,
            "Link armed both sides with no endpoint anywhere" );
    Check ( oLinkUp.Wait ( 10000 ), "linked peer came up" );
    Check ( lkB.sendText ( L"Lk", L"ping", L"linked" ) == S_OK,
            "sendText over a linked edge returned S_OK" );
    Check ( oLinkMsg.Wait ( 5000 ) && strLink == "linked",
            "linked payload round-tripped" );

    // A second Link for one pair is an error, not a silent no-op -- and it
    // must leave the working edge alone rather than half-rearm it.
    Check ( net.link ( L"Lk", L"Lk.Node" ) == p2pf::P2PF_E_CON_DUPLICATE,
            "Link twice for one pair is rejected" );
    Check ( lkB.isPeerUp ( L"Lk" ), "the existing edge is untouched" );

    Check ( net.link ( L"Lk", L"No.Such.Hub" ) == p2pf::P2PF_E_NO_HUB,
            "Link to an address no live hub answers to is rejected" );
    Check ( net.link ( L"Lk", L"Lk" ) == E_INVALIDARG,
            "Link of a hub to itself is rejected" );
    Check ( net.link ( L"Lk", L"tcp://:7788" ) == E_INVALIDARG,
            "Link with an endpoint in an address slot is rejected" );
    Check ( net.link ( L"Lk", L"Lk.Other", L"udp://x:1" )
              == p2pf::P2PF_E_ENDPOINT,
            "Link with an unparseable endpoint is rejected" );

    // Serial is the one endpoint Link can never express: a null-modem link is
    // two DIFFERENT local devices, each opened exclusively, and Link carries
    // one endpoint for both sides.
    Check ( net.link ( L"Lk", L"Lk.Other", L"serial://COM5" )
              == p2pf::P2PF_E_ENDPOINT,
            "Link with a serial endpoint is refused, not half-armed" );

    // -----------------------------------------------------------------
    // 14b. A SECURE HUB: authentication, per hub               (ABI 11)
    //
    // What this section is really checking is that the security is ON
    // rather than that the link works. A link that works is what section
    // 14 already proved, and a secure hub that quietly fell back to a
    // plain one would pass every message check here -- so the assertions
    // that matter are the POSTURE ones: ARMED, and not merely REQUIRED.
    //
    // The keys land in a "p2p" folder beside TargetFacade.dll and are
    // FOUND rather than regenerated on the next run, which is deliberate:
    // a first-run helper that rotated on restart would change every hub's
    // identity behind the operator's back. Deleting that folder
    // re-provisions from scratch.
    // -----------------------------------------------------------------
    {
      std::printf ( "\n-- a secure hub --\n" );

      Flag oSecUp, oSecMsg, oThirdUp;
      std::string strSec;

      p2pf::Hub scA = net.createHub ( L"Sec",      p2pf::P2PF_HUB_SECURE );
      p2pf::Hub scB = net.createHub ( L"Sec.Node", p2pf::P2PF_HUB_SECURE );

      // BEFORE ANY LINK. The hub holds its keys from the moment it exists
      // -- CAN_SIGN, CAN_OPEN and a revocation position -- and requires
      // nothing yet, because an allow-list that lists nobody refuses
      // everybody and the kernel will not start such a hub at all.
      const unsigned int uFresh = scA.securityFlags ( );
      Check ( ( uFresh & p2pf::P2PF_SEC_CAN_SIGN   ) &&
              ( uFresh & p2pf::P2PF_SEC_CAN_OPEN   ) &&
              ( uFresh & p2pf::P2PF_SEC_REVOCATION ),
              "a secure hub holds both keys and a revocation position at birth" );
      Check ( ( uFresh & p2pf::P2PF_SEC_REQUIRED ) == 0,
              "...and requires nothing yet, having nobody to require it of" );

      scA.onTopic  ( L"ping", [&](const p2pf::Message& m)
                     {
                       const wchar_t *w = m.text();
                       while ( w && *w ) strSec += (char)*w++;
                       oSecMsg.Set();
                     } );
      scB.onPeerUp ( [&](const wchar_t*){ oSecUp.Set(); } );

      // PLAIN Link, and that is the point: the verb did not change. Both
      // hubs said what they were when they were CREATED, so this call has
      // nothing left to decide.
      Check ( net.link ( L"Sec", L"Sec.Node" ) == S_OK,
              "Link armed both sides, trading their keys as it went" );
      Check ( oSecUp.Wait ( 10000 ),
              "...and the peer came up, which means the SIGNED login completed" );
      Check ( scB.sendText ( L"Sec", L"ping", L"secured" ) == S_OK &&
              oSecMsg.Wait ( 5000 ) && strSec == "secured",
              "...and a payload round-tripped over the cyphered session" );

      // THE CHECK THAT SEPARATES THIS FROM SECTION 14. ARMED and not just
      // REQUIRED: a hub can require authentication and be unable to
      // perform it, and that combination refuses every peer rather than
      // authenticating any.
      const unsigned int uWant = p2pf::P2PF_SEC_REQUIRED
                               | p2pf::P2PF_SEC_ARMED
                               | p2pf::P2PF_SEC_CAN_SIGN
                               | p2pf::P2PF_SEC_CAN_OPEN
                               | p2pf::P2PF_SEC_REVOCATION;
      const unsigned int uA = scA.securityFlags ( );
      const unsigned int uB = scB.securityFlags ( );
      Check ( ( uA & uWant ) == uWant && ( uB & uWant ) == uWant,
              "both hubs require auth, CAN enforce it, and hold both keys" );

      // Sealing is deliberately NOT required -- it is a property of an
      // origin and a destination, and this flag secures a hub and its
      // EDGES. See IP2PHub::GetSecurityInfo, "what it deliberately does
      // not turn on".
      Check ( ( uA & p2pf::P2PF_SEC_SEALED ) == 0,
              "...and sealing is left off, which is the honest scope" );

      const std::wstring fpA = scA.securityFingerprint ( );
      const std::wstring fpB = scB.securityFingerprint ( );
      Check ( !fpA.empty() && !fpB.empty() && fpA != fpB,
              "each hub has its own identity fingerprint" );

      // INCREMENTAL. A second link off the same hub must find the identity
      // it already holds and ADD to the allow-list rather than replace it
      // -- so Sec.Node must still be up afterwards.
      p2pf::Hub scC = net.createHub ( L"Sec.Third", p2pf::P2PF_HUB_SECURE );
      scC.onPeerUp ( [&](const wchar_t*){ oThirdUp.Set(); } );
      Check ( net.link ( L"Sec", L"Sec.Third" ) == S_OK &&
              oThirdUp.Wait ( 10000 ),
              "a second secure link off the same hub comes up too" );
      Check ( scA.securityFingerprint ( ) == fpA,
              "...without rotating the identity it already had" );
      Check ( scB.isPeerUp ( L"Sec" ),
              "...and the first secure link is untouched" );

      // BOTH SECURE OR NEITHER. Enforcement is hub-wide in the kernel with
      // no per-connection override, so a secure hub demands a login a
      // plain one holds no key to produce. That pair is refused before
      // anything is armed rather than armed into a link that can never
      // come up.
      //
      // Lk and Lk.Node are section 14's PLAIN pair and are still up, so
      // the rule is checked in both directions without minting four more
      // hubs (the kernel allows 16 per process, and this suite is not the
      // only section that wants some).
      Check ( net.link ( L"Sec", L"Lk.Node" ) == p2pf::P2PF_E_SECURITY,
              "a secure hub cannot be linked to a plain one" );
      Check ( net.link ( L"Lk", L"Sec.Third" ) == p2pf::P2PF_E_SECURITY,
              "...and it is refused the same way from the other side" );
      Check ( lkB.isPeerUp ( L"Lk" ),
              "...with the plain edge that made it plain still up" );

      // LISTEN/CONNECT ON A SECURE HUB read the far end's PUBLISHED key
      // files, because that end may be in another process and there is no
      // other way to learn a public point. Sec.Fifth is a hub that has
      // never existed, so it has published nothing, and the refusal names
      // the file it wanted rather than arming an unauthenticated peer.
      Check ( scA.listen ( L"Sec.Fifth", nullptr ) == p2pf::P2PF_E_SECURITY,
              "Listen to a peer with no published key is refused, not armed" );

      // ...and the same call SUCCEEDS for a peer that HAS published, which
      // is the whole cross-process story: Sec.Node wrote "Sec.Node.key.pub"
      // and "Sec.Node.agree.pub" into the security directory when it was
      // created, and on another machine those two files are what an
      // operator copies across. Here they are already in place.
      p2pf::Hub scD = net.createHub ( L"Sec.Node.Leaf", p2pf::P2PF_HUB_SECURE );
      Check ( SUCCEEDED ( scB.listen ( L"Sec.Node.Leaf", nullptr ) ),
              "Listen to a peer that HAS published its key is armed" );
      Check ( ( scB.securityFlags ( ) & p2pf::P2PF_SEC_ARMED ) != 0,
              "...and the hub is still armed afterwards" );

      // A PATTERN LISTENER is the one arm whose peer cannot be provisioned
      // -- there is no key filed under "Sec.*" -- so it is admitted on a
      // different question: is this hub ALREADY authenticating? Sec is, so
      // it may have one. The kernel never sees the pattern in its auth path;
      // the allow-list is keyed on the address off the wire and the pattern
      // only filters the name a peer CLAIMS, so this narrows who may knock
      // rather than widening who gets in.
      Check ( SUCCEEDED ( scA.listen ( L"Sec.Deep.*", L"tcp://:7831" ) ),
              "a secure hub that is already armed may take a wildcard listener" );
      Check ( ( scA.securityFlags ( ) & p2pf::P2PF_SEC_ARMED ) != 0,
              "...and it is still enforcing afterwards" );

      // ...but a wildcard may not be a secure hub's FIRST arm. scD holds keys
      // and trusts nobody -- Sec.Node listened for IT, not the other way round
      // -- so it has never turned enforcement on, and a hub whose only
      // listener is a pattern would sit there accepting anyone at all. That is
      // the one outcome the flag exists to rule out, so it is refused.
      Check ( ( scD.securityFlags ( ) & p2pf::P2PF_SEC_REQUIRED ) == 0,
              "a secure hub that has trusted nobody is not enforcing yet" );
      Check ( scD.listen ( L"Sec.Node.Leaf.*", L"tcp://:7832" )
                == p2pf::P2PF_E_SECURITY,
              "...so a wildcard cannot be its first arm: nothing to enforce with" );

      // A PLAIN hub is untouched by every line above -- including the
      // wildcard rule, which is why section 15 can still arm "Rd.Deep.*".
      Check ( lkA.securityFlags ( ) == 0 && lkA.securityFingerprint ( ).empty ( ),
              "a plain hub holds no identity and enforces nothing" );
      Check ( SUCCEEDED ( lkA.listen ( L"Lk.Any.*", L"tcp://:7833" ) ),
              "...and takes a wildcard listener with no provisioning at all" );

      // The directory cannot move once a hub holds a key out of it: that
      // hub's identity would still be the old one and nothing would say so.
      Check ( net.setSecurityDir ( L"." ) == p2pf::P2PF_E_SECURITY,
              "the security directory is fixed once anything is provisioned" );

      scA.close(); scB.close(); scC.close(); scD.close();
    }

    // -----------------------------------------------------------------
    // 15. The read side: what a hub can say about itself
    // -----------------------------------------------------------------
    std::printf ( "\n-- read side --\n" );

    // `rd` is the parent of rd.Child (which it dials) and of rd.Wait (which
    // it only expects); rdSib is a sibling, and a wildcard listener will pick
    // up a name nobody armed. One hub, every row shape.
    Flag oRdUp;
    p2pf::Hub rd  = net.createHub ( L"Rd" );
    p2pf::Hub rdc = net.createHub ( L"Rd.Child" );
    rd.onPeerUp ( [&](const wchar_t*){ oRdUp.Set(); } );

    Check ( rd.listen  ( L"Rd.Wait",  L"tcp://:7807" ) == S_OK,
            "read side: armed a listener that nobody will answer" );
    Check ( rd.listen  ( L"Rd.Deep.*", L"tcp://:7808" ) == S_OK,
            "read side: armed a pattern listener" );
    Check ( rd.listen  ( L"Sibling.Elsewhere", L"tcp://:7809" )
              == p2pf::P2PF_S_UNRELATED_LINK,
            "read side: armed an unrelated link" );
    // Explicit endpoint: the caller states the DIAL and the listener gets it
    // with the host dropped -- which the read side is about to prove.
    Check ( net.link ( L"Rd", L"Rd.Child", L"tcp://127.0.0.1:7806" ) == S_OK,
            "read side: linked a live child over an explicit TCP endpoint" );
    Check ( oRdUp.Wait ( 10000 ), "read side: the child came up" );

    unsigned int uCount = 0;
    Check ( rd.raw()->GetConCount ( &uCount ) == S_OK && uCount == 4,
            "GetConCount counts every peer the hub knows" );

    std::vector<p2pf::Hub::Con> aCons = rd.cons();
    Check ( aCons.size() == 4, "cons() returned every row" );

    // Endpoints round-trip: what comes back out is what would go back in.
    Check ( rd.endpointFor ( L"Rd.Wait" ) == L"tcp://:7807",
            "GetEndpoint round-trips into Listen" );
    Check ( rd.endpointFor ( L"Rd.Child" ) == L"tcp://:7806",
            "Link's listener side recorded the dial with its host dropped" );
    {
      unsigned int cchNone = 0;
      Check ( rd.raw()->GetEndpoint ( L"Never.Heard.Of", nullptr, &cchNone )
                == p2pf::P2PF_E_UNRESOLVED,
              "GetEndpoint on an unknown peer is P2PF_E_UNRESOLVED" );
    }

    // The half of the read side with the diagnostic value: the relation.
    // "Sibling.Elsewhere" connects, logs in and looks healthy; only this says
    // it can never be routed through.
    for ( size_t i = 0; i < aCons.size(); ++i )
    {
      const p2pf::Hub::Con& c = aCons[i];
      if ( c.peer == L"Rd.Child" )
        Check ( ( c.flags & p2pf::P2PF_REL_DESCENDANT ) && c.listening() && c.up(),
                "Link's listener side reads back as a descendant, listening, up" );
      else if ( c.peer == L"Rd.Wait" )
        Check ( ( c.flags & p2pf::P2PF_REL_DESCENDANT ) && c.listening() && !c.up(),
                "an armed-but-unanswered listener reads back as not up" );
      else if ( c.peer == L"Rd.Deep.*" )
        Check ( ( c.flags & p2pf::P2PF_REL_PATTERN ) != 0,
                "a pattern peer is reported as unclassifiable, not guessed" );
      else if ( c.peer == L"Sibling.Elsewhere" )
        Check ( c.unrelated(),
                "the unrelated link is visible as unrelated afterwards" );
      else
        Check ( false, "unexpected row in cons()" );
    }

    // Exactly one relation bit, always -- callers switch on the mask.
    bool bOneEach = true;
    for ( size_t i = 0; i < aCons.size(); ++i )
    {
      unsigned int r = aCons[i].flags & p2pf::P2PF_REL_MASK;
      if ( r == 0 || ( r & ( r - 1 ) ) != 0 ) bOneEach = false;
    }
    Check ( bOneEach, "exactly one relation bit is set on every row" );

    // A peer nobody armed: the dialing side of the link learned "Rd" at login.
    {
      std::vector<p2pf::Hub::Con> aChild = rdc.cons();
      bool bLearned = false;
      for ( size_t i = 0; i < aChild.size(); ++i )
        if ( aChild[i].peer == L"Rd" )
          bLearned = aChild[i].dialing() && aChild[i].endpoint.size() > 0 &&
                     ( aChild[i].flags & p2pf::P2PF_REL_ANCESTOR ) != 0;
      Check ( bLearned, "the dialing side reports dial + ancestor + its endpoint" );
    }

    // The buffer protocol, which is the part most likely to be got wrong.
    {
      unsigned int cch = 0;
      Check ( rd.raw()->Describe ( nullptr, &cch ) == S_OK && cch > 1,
              "a NULL buffer asks for the size and gets it" );

      unsigned int cchShort = 4;
      wchar_t      awTiny[4] = { L'z', L'z', L'z', L'z' };
      HRESULT hrShort = rd.raw()->Describe ( awTiny, &cchShort );
      Check ( hrShort == HRESULT_FROM_WIN32 ( ERROR_MORE_DATA ),
              "a short buffer is ERROR_MORE_DATA" );
      Check ( cchShort == cch,
              "...and still reports the size actually needed" );
      Check ( awTiny[0] == L'z',
              "...and writes nothing: a partial string looks like a value" );

      std::vector<wchar_t> aExact ( cch );
      unsigned int cchExact = cch;
      Check ( rd.raw()->Describe ( &aExact[0], &cchExact ) == S_OK &&
              ::wcslen ( &aExact[0] ) == cch - 1,
              "an exactly-sized buffer fits, terminator included" );
    }

    // The format is pinned: it is a diffable log line, not free text.
    {
      std::wstring strDesc = rd.describe();
      Check ( strDesc.compare ( 0, 11, L"address=Rd\n" ) == 0,
              "Describe leads with the hub's own address" );
      Check ( strDesc.find ( L"con=Rd.Child\ttcp://:7806\tlisten,up,descendant\n" )
                != std::wstring::npos,
              "Describe: peer, endpoint and flags, tab separated" );
      Check ( strDesc.find ( L"con=Rd.Wait\ttcp://:7807\tlisten,descendant\n" )
                != std::wstring::npos,
              "Describe omits 'up' for a listener nobody answered" );
      Check ( strDesc.find ( L"con=Sibling.Elsewhere\ttcp://:7809\tlisten,unrelated\n" )
                != std::wstring::npos,
              "Describe names the unrelated link" );
      std::wprintf ( L"\n%s\n", strDesc.c_str() );
    }

    // -----------------------------------------------------------------
    // 16. Closing a hub that owns an armed-but-never-connected listener
    // -----------------------------------------------------------------
    // The one section that closes its own hubs ON PURPOSE, mid-suite -- which
    // is exactly the thing every other section avoids (see the teardown note
    // below), and the reason this is a regression rather than a feature test.
    //
    // `listen(peer, nullptr)` arms a Dmx SERVICE by construction, and a Dmx
    // service registers itself in a PROCESS-GLOBAL kernel list. If nobody ever
    // dials it, it never logs in; DrainCons used to retire only peers that had
    // logged in, so such a connection survived into the kernel's own teardown
    // sweep, which frees it WITHOUT removing it from that global list. The
    // entry then pointed at freed memory -- and P2PeerConDmx::Listen's
    // duplicate guard compares by POINTER, so the next Dmx listener the
    // allocator happened to place at that address threw "Duplicate listen
    // attempted on single connection" on a hub that had done nothing wrong.
    //
    // The loop runs several times because the failure needs the allocator to
    // reuse the address; one iteration would make this a coin toss.
    std::printf ( "\n-- unconnected-listener teardown --\n" );

    int nArmed = 0, nLinked = 0;
    for ( int i = 0; i < kDrainRounds; ++i )
    {
      wchar_t wszHub[64], wszGhost[64];
      std::swprintf ( wszHub,   64, L"Drn%d",       i );
      std::swprintf ( wszGhost, 64, L"Drn%d.Ghost", i );

      // The shape that used to corrupt the heap: a Dmx service armed for a
      // peer that never dials, then the hub closed under it.
      {
        p2pf::Hub ghost = net.createHub ( wszHub );
        if ( ghost.listen ( wszGhost, nullptr ) == S_OK ) ++nArmed;
        ghost.close();
      }

      // And the functional consequence, measured rather than assumed: a Dmx
      // link created AFTERWARDS must still arm AND come up. S_OK from listen
      // is not enough on its own -- arming happens on the peer hub's pump
      // thread, so a listener that fails there returns S_OK here and simply
      // never answers. Waiting for the login is what makes this a real check.
      wchar_t wszA[64], wszB[64];
      std::swprintf ( wszA, 64, L"Dl%d",      i );
      std::swprintf ( wszB, 64, L"Dl%d.Node", i );

      Flag oUp;
      p2pf::Hub a = net.createHub ( wszA );
      p2pf::Hub b = net.createHub ( wszB );
      b.onPeerUp ( [&](const wchar_t*){ oUp.Set(); } );
      if ( net.link ( wszA, wszB, nullptr ) == S_OK && oUp.Wait ( 10000 ) )
        ++nLinked;
      b.close();
      a.close();
    }

    Check ( nArmed == kDrainRounds,
            "arm an endpoint-less listener, never dial it, close the hub" );

    // Reaching this line at all is the FACADE half of the fix: no heap
    // corruption, no abort. Pre-fix this suite died on round 3, with the
    // hub's connections left for the kernel's teardown sweep to walk and free
    // mid-mutation.
    Check ( true, "...without corrupting the heap or aborting the process" );

    // And this is the KERNEL half. P2PeerConDmx::Listen registers `this` in
    // the process-global g_oCListP2PeerConDmx and, until this was fixed,
    // nothing ever removed it -- so every Dmx listener ever armed left a
    // permanent dangling pointer, Connect's rendezvous scan dereferenced it
    // on every later dial, and Listen's pointer-comparison guard threw on the
    // next connection the allocator placed at a recycled address.
    // Measured with the facade half alone: 3 of 8.
    Check ( nLinked == kDrainRounds,
            "...and every in-process link created afterwards still comes up" );

    // -----------------------------------------------------------------
    // 17. A resolved dial retries the rendezvous, and reports giving up
    // -----------------------------------------------------------------
    // The other half of the Dmx story, and the one that was never a crash --
    // just a link that quietly never happened.
    //
    // An endpoint-less connect() resolves to an in-process Dmx service, and
    // that dial used to be ONE SHOT. It could not fail for the reason one-shot
    // was chosen to catch: the resolution refuses outright unless a live
    // sibling hub has already recorded a listener expecting us, so "the
    // service is misconfigured" is off the table before anything is dialled.
    // What was left was pure timing -- the record is written BEFORE the
    // listener's connection is posted, and the arming itself happens later on
    // that hub's pump -- and timing is what one shot handles worst. The dial
    // threw on a missed rendezvous and vanished: connect() had already
    // returned S_OK, the peer had never been up so no onPeerDown could fire,
    // and nothing whatsoever reached the client.
    std::printf ( "\n-- resolved-dial retry --\n" );

    // WHAT THIS SECTION DOES NOT DO, stated up front because the omission is
    // the interesting part. It does not MEASURE a lost rendezvous. Arming a
    // millisecond before a second boundary (ParkBeforeSecondBoundary) shrinks
    // the kernel's spin to almost nothing, and the listener STILL wins every
    // time: instrumented, RetryDialDmx::Connect ran exactly once per link in
    // every round observed. The window is real -- it is the whole reason the
    // one-shot dial was wrong -- but it is narrower than anything this suite
    // can force through the public API, because the listener's P2P_Listen is
    // posted before the dialer's P2P_Startup and both are microsecond-scale.
    // Nor can it reach the give-up report: a redial only ever starts from a
    // close the DIALING hub observes, and closing the far hub does not produce
    // one (measured -- the kernel wakes the paired con's pending recv, and a
    // hub that has already retired its cons has none pending). So the states
    // this section pins are the reachable ones, and the retry itself is
    // asserted by construction rather than by measurement.
    //
    // (a) The narrowest window the API can arrange, run several times. Every
    //     one must come up. Pre-fix this could only ever be luck; it also
    //     covers RetryDialDmx's construction, which is field-by-field rather
    //     than through ClientFactory and would silently mis-build the
    //     connection if the protocol object or the mode were wrong.
    int nRaced = 0;
    for ( int i = 0; i < kRaceRounds; ++i )
    {
      wchar_t wszA[64], wszB[64];
      std::swprintf ( wszA, 64, L"Rc%d",      i );
      std::swprintf ( wszB, 64, L"Rc%d.Node", i );

      Flag oUp;
      p2pf::Hub a = net.createHub ( wszA );
      p2pf::Hub b = net.createHub ( wszB );
      b.onPeerUp ( [&](const wchar_t*){ oUp.Set(); } );

      // Deliberately NOT net.link(): Link exists to arm these two in the order
      // that works, and this check is about the order that does not.
      ParkBeforeSecondBoundary ( 2 );
      HRESULT hrL = a.listen  ( wszB, nullptr );
      HRESULT hrC = b.connect ( wszA, nullptr );

      if ( hrL == S_OK && hrC == S_OK && oUp.Wait ( 15000 ) )
        ++nRaced;

      b.close();
      a.close();
    }
    Check ( nRaced == kRaceRounds,
            "a resolved dial armed in the narrowest window still comes up" );

    // (b) The policy boundary, which is the half that CAN be pinned exactly.
    //     Retry was given to the resolved dial and to nothing else, so an
    //     endpoint the caller spells out must behave exactly as it always has:
    //     dial once, and stay down if the far side was not listening yet.
    //     Same shape as (a) -- dialer armed before listener -- and the
    //     opposite outcome, on purpose.
    Flag oExplicitUp;

    p2pf::Hub exA = net.createHub ( L"Ex" );
    p2pf::Hub exB = net.createHub ( L"Ex.Node" );
    exB.onPeerUp ( [&](const wchar_t*){ oExplicitUp.Set(); } );

    Check ( exB.connect ( L"Ex", L"dmx://ExLate" ) == S_OK,
            "an explicit dmx:// dial arms with nothing listening" );

    // The kernel prints its own report of the failed rendezvous somewhere
    // around here ("Connection ... failed / This P2PeerHub not yet running?").
    // That blob is not noise from this test -- it IS the one-shot dial dying,
    // and a facade dial suppresses it by overriding HasDroppedOut.
    ::Sleep ( 1500 );        // past the kernel's rendezvous spin, whatever it was
    Check ( exA.listen ( L"Ex.Node", L"dmx://ExLate" ) == S_OK,
            "...and the service it wanted appears afterwards" );

    Check ( !oExplicitUp.Wait ( 4000 ),
            "...but a typed endpoint is still ONE SHOT: the link stays down" );

    exB.close();
    exA.close();
    Check ( true, "...and both hubs close cleanly afterwards" );

    // -----------------------------------------------------------------
    // 18. The deployment map -- an endpoint that is nowhere in the code
    // -----------------------------------------------------------------
    // Making the endpoint a string was half of what the arming pair was for.
    // This is the other half: until the map existed, the only place to put
    // that string was the call site, so "the endpoint is configuration" was
    // true of the TYPE and false of the API -- nothing could read an ini, an
    // argv or a registry value into arming.
    //
    // One table, keyed by ADDRESS, holding the DIAL form, serving both verbs:
    // a connect looks up its PEER, a listen looks up ITS OWN address and drops
    // the host. Consulted BEFORE the in-process convention -- what a
    // deployment stated beats what a process can infer.
    std::printf ( "\n-- the deployment map --\n" );

    // It arrived as ABI 5, and ABI 5 is APPEND-ONLY -- three methods on the end
    // of IP2PNetwork, nothing moved, IP2PHub untouched. So the factory takes 4
    // as well, and a binary built against the previous header still runs here.
    // Worth pinning rather than asserting in a comment: it is the one property
    // of an append-only change that can quietly stop being true.
    {
      p2pf::IP2PNetwork *pOld = nullptr;
      HRESULT hrOld = P2PF_CreateNetwork ( 4, &pOld );
      Check ( hrOld == S_OK && pOld != nullptr,
              "the factory still accepts ABI 4: appending did not break a v4 client" );
      if ( pOld ) pOld->Release();

      p2pf::IP2PNetwork *pAncient = nullptr;
      Check ( P2PF_CreateNetwork ( 3, &pAncient ) == p2pf::P2PF_E_ABI_MISMATCH &&
              pAncient == nullptr,
              "...and still refuses 3, which the hard cut left no prefix for" );
    }

    // Exactly what would come out of a file: comments, blanks, indentation,
    // mixed case, and one entry that is never used (a map describes a
    // deployment, not this test).
    const wchar_t *kMap =
        L"# peers.ini -- where each address lives\n"
        L"\n"
        L"  Cfg.Server = TCP://127.0.0.1:7812\n"
        L"; a semicolon comments too\n"
        L"Cfg.Elsewhere = pipe://P2PmsgCfgElsewhere\n";

    unsigned int uBad = 999;
    Check ( net.setEndpointMap ( kMap, &uBad ) == S_OK && uBad == 0,
            "a map with comments, blanks and indentation is accepted" );

    // Canonically, not as typed: "TCP://" came back "tcp://", so the read side
    // matches what the arming verbs would report.
    Check ( net.endpointFor ( L"Cfg.Server" ) == L"tcp://127.0.0.1:7812",
            "...and reads back canonically, not as it was spelled" );
    Check ( net.endpointFor ( L"Cfg.Nobody" ).empty(),
            "...with nothing invented for an address it does not mention" );

    // THE POINT OF THE WHOLE TIER: neither call below names an endpoint, and
    // neither hub is reachable by the in-process convention (the map answers
    // first), yet a TCP link comes up.
    Flag oCfgUp;
    p2pf::Hub cfgSrv = net.createHub ( L"Cfg.Server" );
    p2pf::Hub cfgCli = net.createHub ( L"Cfg.Client" );
    cfgCli.onPeerUp ( [&](const wchar_t*){ oCfgUp.Set(); } );

    Check ( Armed ( cfgSrv.listen  ( L"Cfg.Client", nullptr ) ) &&
            Armed ( cfgCli.connect ( L"Cfg.Server", nullptr ) ) &&
            oCfgUp.Wait ( 10000 ),
            "listen(peer,NULL) + connect(peer,NULL) link up over CONFIGURED tcp" );

    // The two halves of one entry. A listener cannot know which host a dialer
    // will use to reach it, so the stored form is the dial and the listen is
    // that with the host removed -- the same conversion Link performs.
    Check ( cfgSrv.endpointFor ( L"Cfg.Client" ) == L"tcp://:7812",
            "...the listener took the map entry with the host dropped" );
    Check ( cfgCli.endpointFor ( L"Cfg.Server" ) == L"tcp://127.0.0.1:7812",
            "...and the dialer took it verbatim" );

    // Precedence, stated as a difference rather than asserted in the abstract:
    // this pair is exactly the shape section 13 resolves in-process, and the
    // listener would have armed dmx://P2PF|Cfg.Server|Cfg.Client if the map
    // had not answered first.
    Check ( cfgSrv.endpointFor ( L"Cfg.Client" ).rfind ( L"dmx://", 0 ) != 0,
            "...instead of the dmx:// name the in-process tier would derive" );

    // A bad line is refused with the number an editor shows -- blanks and
    // comments counted -- and the map that was already loaded is untouched.
    // All-or-nothing matters here more than anywhere else in the ABI: half a
    // deployment map makes the entries that DID load look like a routing
    // problem rather than a typo.
    struct BadMap { const wchar_t *text; unsigned int line; const char *what; };
    static const BadMap kBad[] =
    {
      { L"A = tcp://h:1\nB\n",                    2, "a line with no '='" },
      { L"A = tcp://h:1\n\n= tcp://h:2\n",         3, "an empty address" },
      { L"# c\ntcp://h:1 = tcp://h:2\n",           2, "an endpoint in the address slot" },
      { L"A = tcp://h:1\nB = htp://nope\n",        2, "an endpoint the grammar refuses" },
      { L"A = tcp://h:1\nB = tcp://:2\n",          2, "a dial form with no host" },
      { L"; c\n\nA = serial://COM5\n",             3, "serial://, which one entry cannot express" },
      { L"A = tcp://h:1\n# c\nA = pipe://x\n",     3, "the same address twice" },
    };
    int nBadOk = 0;
    for ( size_t i = 0; i < sizeof(kBad)/sizeof(kBad[0]); ++i )
    {
      unsigned int uLine = 0;
      if ( net.setEndpointMap ( kBad[i].text, &uLine ) == p2pf::P2PF_E_ENDPOINT &&
           uLine == kBad[i].line )
        ++nBadOk;
      else
        std::printf ( "    (rejection %d: %s -- line %u, wanted %u)\n",
                      (int)i, kBad[i].what, uLine, kBad[i].line );
    }
    Check ( nBadOk == (int)( sizeof(kBad)/sizeof(kBad[0]) ),
            "every malformed map is refused at the line an editor would show" );
    Check ( net.endpointFor ( L"Cfg.Server" ) == L"tcp://127.0.0.1:7812",
            "...and a refused map leaves the loaded one exactly as it was" );

    // The address slot is guarded the same way the arming verbs guard theirs.
    Check ( net.setEndpoint ( L"",          L"tcp://h:1" ) == E_INVALIDARG &&
            net.setEndpoint ( L"tcp://h:1", L"tcp://h:2" ) == E_INVALIDARG,
            "SetEndpoint rejects an empty or endpoint-shaped address" );
    Check ( net.setEndpoint ( L"Cfg.Late",  L"serial://COM5" ) == p2pf::P2PF_E_ENDPOINT,
            "...and refuses serial for one entry, as Link does" );

    // Single entries: set, read, and clear with an empty endpoint.
    Check ( net.setEndpoint ( L"Cfg.Late", L"pipe://P2PmsgCfgLate" ) == S_OK &&
            net.endpointFor ( L"Cfg.Late" ) == L"pipe://P2PmsgCfgLate",
            "SetEndpoint adds one entry without disturbing the rest" );
    Check ( net.setEndpoint ( L"Cfg.Late", nullptr ) == S_OK &&
            net.endpointFor ( L"Cfg.Late" ).empty() &&
            net.endpointFor ( L"Cfg.Server" ) == L"tcp://127.0.0.1:7812",
            "...and an empty endpoint removes it, leaving the others" );

    // The caller-sized buffer protocol, same as the hub's read side.
    {
      const unsigned int uWant =
          (unsigned int)::wcslen ( L"tcp://127.0.0.1:7812" ) + 1;

      unsigned int cch  = 0;
      HRESULT hrSz = net.raw()->GetEndpointFor ( L"Cfg.Server", nullptr, &cch );

      wchar_t      wszSmall[4] = { 0 };
      unsigned int cchSmall    = 4;
      HRESULT hrShort = net.raw()->GetEndpointFor ( L"Cfg.Server", wszSmall, &cchSmall );

      std::wstring sFit ( uWant, L'\0' );
      unsigned int cchFit = uWant;
      HRESULT hrFit = net.raw()->GetEndpointFor ( L"Cfg.Server", &sFit[0], &cchFit );

      Check ( hrSz    == S_OK && cch == uWant &&
              hrShort == HRESULT_FROM_WIN32 ( ERROR_MORE_DATA ) &&
              cchSmall == uWant && wszSmall[0] == 0 &&   // nothing written
              hrFit   == S_OK &&
              sFit.compare ( 0, uWant - 1, L"tcp://127.0.0.1:7812" ) == 0,
              "GetEndpointFor honours the size query / short-buffer protocol" );
    }

    // Put the process back the way it was found: the map is network-wide, so
    // leaving it set would quietly re-point anything armed after this.
    cfgCli.close();
    cfgSrv.close();
    Check ( net.setEndpointMap ( nullptr ) == S_OK &&
            net.endpointFor ( L"Cfg.Server" ).empty(),
            "empty text clears the map" );

    // -----------------------------------------------------------------
    // 9. Teardown -- destructors close the hubs, then the kernel
    // -----------------------------------------------------------------
    // Everything closes HERE rather than section by section -- now a habit
    // rather than a requirement. It used to be a requirement: closing a hub
    // that still owned an armed-but-never-connected Dmx listener (sections 11
    // and 13 create several) corrupted the heap and left a stale entry in the
    // kernel's global Dmx service list. Section 16 is the regression for
    // exactly that, and it closes its hubs mid-suite on purpose.
    std::printf ( "\n-- teardown --\n" );
    rdc.close();
    rd.close();
    lkB.close();
    lkA.close();
    autoB.close();
    autoA.close();
    uniB.close();
    uniA.close();
    pipeB.close();
    pipeA.close();
    dmxB.close();
    dmxA.close();
    client.close();
    server.close();
    Check ( true, "all hubs closed without hanging" );

    // -----------------------------------------------------------------
    // 19. A whole process from one block of text
    // -----------------------------------------------------------------
    // Opus C5, and deliberately a LAYER ABOVE the facade -- header-only,
    // composed entirely from CreateHub + SetEndpointMap + Link, adding nothing
    // to the ABI. It waited for section 18 because the map is the half of it
    // that had to live inside the DLL.
    //
    // AFTER the teardown above, and not for tidiness: the kernel caps a
    // process at 16 hubs, this suite holds 14 of them open until teardown by
    // design (see the note there), and a three-hub chain plus a two-hub pair
    // does not fit in what is left. Running here also happens to be the
    // honest demonstration -- the helper standing on its own in an empty
    // process, which is how a client would actually use it.
    //
    // The shape being demonstrated is the one the plan predicted: the eleven
    // _Targetcore_UseExamples mesh harnesses are all "create some hubs, arm some
    // edges, exchange a message", and that part becomes data.
    std::printf ( "\n-- topology from text --\n" );

    // A CHAIN, not a full mesh, on purpose: A-B and B-C only. Send already
    // routes multi-hop, so linking every pair costs connections and buys
    // nothing -- and a chain proves the routing rather than hiding it behind
    // a direct edge.
    const wchar_t *kTopo =
        L"# a three-hub chain: Top -> Top.Mid -> Top.Mid.Leaf\n"
        L"\n"
        L"hub  Top\n"
        L"hub  Top.Mid\n"
        L"hub  Top.Mid.Leaf\n"
        L"\n"
        L"; edges are listener -> dialer, and the arrow is not decoration\n"
        L"link Top     -> Top.Mid\n"
        L"link Top.Mid -> Top.Mid.Leaf\n";

    p2pf::Topology topo;
    unsigned int   uTopoBad = 999;
    Check ( p2pf::Topology::parse ( kTopo, topo, &uTopoBad ) == S_OK &&
            uTopoBad == 0 &&
            topo.hubs().size() == 3 && topo.edges().size() == 2,
            "a topology parses into 3 hubs and 2 edges" );

    // Parsing has no side effects, which is what makes a bad file safe.
    bool bMade = topo.createHubs ( net ) == S_OK &&
                 topo.hub ( L"Top" )          != nullptr &&
                 topo.hub ( L"Top.Mid" )      != nullptr &&
                 topo.hub ( L"Top.Mid.Leaf" ) != nullptr &&
                 topo.hub ( L"Nope" )         == nullptr;
    Check ( bMade, "createHubs makes every hub and nothing else" );

    Flag         oLeafGot, oChainUp;
    std::wstring strLeafSaw;
    if ( bMade )
    {
      // THE WINDOW THAT JUSTIFIES TWO PHASES: handlers go on here, after the
      // hubs exist and before anything is armed.
      topo.hub ( L"Top.Mid.Leaf" )->onTopic ( L"relay",
          [&](const p2pf::Message& m)
          {
            strLeafSaw = m.text() ? m.text() : L"";
            oLeafGot.Set();
          } );
      topo.hub ( L"Top.Mid" )->onPeerUp ( [&](const wchar_t*){ oChainUp.Set(); } );

      Check ( topo.arm ( net ) == S_OK, "arm() pushes the map and links every edge" );
      Check ( oChainUp.Wait ( 10000 ), "...and the chain comes up" );

      // Wait for the far end of the chain too: Top has no direct edge to
      // Top.Mid.Leaf, so this message has to be ROUTED through Top.Mid.
      for ( int i = 0; i < 200 && !topo.hub ( L"Top.Mid" )->isPeerUp ( L"Top.Mid.Leaf" ); ++i )
        ::Sleep ( 50 );

      Check ( SUCCEEDED ( topo.hub ( L"Top" )->sendText ( L"Top.Mid.Leaf", L"relay",
                                                          L"two hops" ) ) &&
              oLeafGot.Wait ( 10000 ) && strLeafSaw == L"two hops",
              "...and a message crosses BOTH hops with no edge between the ends" );
    }
    else
    {
      Check ( false, "arm() pushes the map and links every edge" );
      Check ( false, "...and the chain comes up" );
      Check ( false, "...and a message crosses BOTH hops with no edge between the ends" );
    }

    // Endpoints come from the same file. `hub A = ep` is a declaration and a
    // map entry in one line; an edge with no endpoint of its own takes the
    // LISTENER's, so this pair links over tcp without either side saying so.
    const wchar_t *kTopoTcp =
        L"hub  Cfg2.Server = tcp://127.0.0.1:7816\n"
        L"hub  Cfg2.Server.Client\n"
        L"at   Cfg2.Absent = pipe://P2PmsgCfg2Absent\n"
        L"link Cfg2.Server -> Cfg2.Server.Client\n";

    p2pf::Topology topo2;
    Check ( p2pf::Topology::parse ( kTopoTcp, topo2 ) == S_OK &&
            topo2.hubs().size() == 2 && topo2.edges().size() == 1 &&
            topo2.mapText().find ( L"Cfg2.Absent" ) != std::wstring::npos,
            "`hub A = ep` is a declaration and a map entry in one line" );

    // The first chain closes before this one opens: 16 hubs is the ceiling and
    // this suite has already spent most of it.
    topo.closeAll();

    Flag oTopo2Up;
    bool bMade2 = topo2.createHubs ( net ) == S_OK &&
                  topo2.hub ( L"Cfg2.Server" )        != nullptr &&
                  topo2.hub ( L"Cfg2.Server.Client" ) != nullptr;
    Check ( bMade2, "createHubs for the tcp topology" );
    if ( bMade2 )
    {
      topo2.hub ( L"Cfg2.Server.Client" )->onPeerUp ( [&](const wchar_t*){ oTopo2Up.Set(); } );
      Check ( topo2.arm ( net ) == S_OK && oTopo2Up.Wait ( 10000 ),
              "...and an edge with no endpoint takes the LISTENER's map entry" );
      Check ( topo2.hub ( L"Cfg2.Server" )->endpointFor ( L"Cfg2.Server.Client" )
                == L"tcp://:7816",
              "...which is tcp here, not the in-process default" );
    }
    else
    {
      Check ( false, "...and an edge with no endpoint takes the LISTENER's map entry" );
      Check ( false, "...which is tcp here, not the in-process default" );
    }
    topo2.closeAll();

    // Structural rejections, each at its own line, and none of them touching
    // the process: parse() creates nothing.
    struct BadTopo { const wchar_t *text; unsigned int line; const char *what; };
    static const BadTopo kBadTopo[] =
    {
      { L"hub A\nwibble B\n",                      2, "an unknown verb" },
      { L"hub A\nhub A\n",                         2, "a hub declared twice" },
      { L"hub A\nhub B\nlink A B\n",               3, "a link with no arrow" },
      { L"hub A\nlink A -> B\n",                   2, "a link to an undeclared hub" },
      { L"hub A\nhub B\nlink A -> A\n",            3, "an edge from a hub to itself" },
      { L"# c\nhub tcp://x:1\n",                   2, "an endpoint in the hub slot" },
      { L"hub A\n\nat A\n",                        3, "an `at` with nothing to say" },
      { L"hub A = tcp://h:1\nat A = pipe://x\n",   2, "two endpoints for one address" },
    };
    int nTopoBadOk = 0;
    for ( size_t i = 0; i < sizeof(kBadTopo)/sizeof(kBadTopo[0]); ++i )
    {
      p2pf::Topology t;
      unsigned int   uL = 0;
      if ( p2pf::Topology::parse ( kBadTopo[i].text, t, &uL ) == p2pf::P2PF_E_ENDPOINT &&
           uL == kBadTopo[i].line )
        ++nTopoBadOk;
      else
        std::printf ( "    (topology rejection %d: %s -- line %u, wanted %u)\n",
                      (int)i, kBadTopo[i].what, uL, kBadTopo[i].line );
    }
    Check ( nTopoBadOk == (int)( sizeof(kBadTopo)/sizeof(kBadTopo[0]) ),
            "every malformed topology is refused at the right line" );

    // A BAD ENDPOINT is not caught by parse() at all -- this header does not
    // own the endpoint grammar, the DLL does, and there is exactly one parser.
    // It surfaces from arm(), and the line number is translated back out of
    // the generated map text into a line number in the caller's file.
    {
      p2pf::Topology tEp;
      unsigned int   uParse = 999, uArm = 0;
      const wchar_t *kBadEp =
          L"# three good lines, then one that is not\n"
          L"hub  Ep.A\n"
          L"hub  Ep.B = htp://nope\n"
          L"link Ep.A -> Ep.B\n";

      Check ( p2pf::Topology::parse ( kBadEp, tEp, &uParse ) == S_OK && uParse == 0,
              "a bad ENDPOINT is not a structural error: parse accepts it" );
      Check ( tEp.createHubs ( net ) == S_OK &&
              tEp.arm ( net, &uArm ) == p2pf::P2PF_E_ENDPOINT && uArm == 3,
              "...arm() reports it, at the line of the ORIGINAL file" );
      tEp.closeAll();
    }

    Check ( net.setEndpointMap ( nullptr ) == S_OK, "topology teardown clears the map" );

    // -----------------------------------------------------------------
    // 20. The accept filter -- the same on every transport
    // -----------------------------------------------------------------
    // `toPeer` on a listen is TWO stamps on the connection: the peer address
    // it will route to, and -- because the second stamp is a P2Padomain -- the
    // filter an arriving login's CLAIMED address is checked against
    // (P2PeerCon.cpp:1876-1886). That filter is the only built-in place a
    // topology error is caught at login rather than diagnosed later.
    //
    // It was not applied uniformly. P2PeerCon's ctor sets m_oP2Padomain, and
    // the Wsa, Pipe and 232 factories set it again explicitly -- but
    // P2PeerConDmx's factories build from the DEFAULT ctor and never set it,
    // so the domain stayed null and the kernel's own guard reads a null domain
    // as "no restriction". Result: a listen(peer, ...) enforced the peer's
    // claimed identity over three transports and not over the fourth -- and
    // the fourth is what an OMITTED endpoint resolves to, i.e. the path the
    // zero-configuration story recommends.
    //
    // Both halves below are the same shape: dial a service the listener is
    // holding, and claim a different address than the one it armed. The Dmx
    // rendezvous matches on the service NAME alone (peer address and owning
    // hub ignored), which is what makes the claim reachable at all.
    std::printf ( "\n-- the accept filter --\n" );

    {
      Flag oWrongUp, oRightUp;

      p2pf::Hub fltSrv = net.createHub ( L"Flt.Expect" );
      p2pf::Hub fltBad = net.createHub ( L"Flt.Wrong" );
      fltSrv.onPeerUp ( [&](const wchar_t*){ oWrongUp.Set(); } );

      // The listener expects Flt.Right and will never see it.
      Check ( Armed ( fltSrv.listen ( L"Flt.Right", L"dmx://P2PmsgFltGate" ) ),
              "a dmx listener armed for one peer" );
      Check ( Armed ( fltBad.connect ( L"Flt.Expect", L"dmx://P2PmsgFltGate" ) ),
              "...and a different hub dials the same service" );

      // Give the handshake every chance to complete before concluding it did
      // not: a false PASS here would be the whole check inverted.
      Check ( !oWrongUp.Wait ( 4000 ) && !fltBad.isPeerUp ( L"Flt.Expect" ),
              "...the login is REFUSED: the claim is outside the armed domain" );

      fltBad.close();
      fltSrv.close();
    }

    // The control, and the reason this is a consistency fix rather than a new
    // feature: over tcp the identical shape has always been refused.
    {
      Flag oTcpWrongUp;

      p2pf::Hub tSrv = net.createHub ( L"Flt2.Expect" );
      p2pf::Hub tBad = net.createHub ( L"Flt2.Wrong" );
      tSrv.onPeerUp ( [&](const wchar_t*){ oTcpWrongUp.Set(); } );

      Check ( Armed ( tSrv.listen ( L"Flt2.Right", L"tcp://:7818" ) ) &&
              Armed ( tBad.connect ( L"Flt2.Expect", L"tcp://127.0.0.1:7818" ) ),
              "the same shape over tcp: armed" );
      Check ( !oTcpWrongUp.Wait ( 4000 ) && !tBad.isPeerUp ( L"Flt2.Expect" ),
              "...refused there too, as it always was -- now they agree" );

      tBad.close();
      tSrv.close();
    }

    // And the filter is a PATTERN, not an equality test, so the wildcard
    // listener section 12 relies on still admits everything it should.
    {
      Flag oPatUp;

      p2pf::Hub pSrv = net.createHub ( L"Pat" );
      p2pf::Hub pKid = net.createHub ( L"Pat.Kid" );
      pSrv.onPeerUp ( [&](const wchar_t*){ oPatUp.Set(); } );

      Check ( Armed ( pSrv.listen  ( L"Pat.*", L"dmx://P2PmsgFltPat" ) ) &&
              Armed ( pKid.connect ( L"Pat",   L"dmx://P2PmsgFltPat" ) ) &&
              oPatUp.Wait ( 10000 ),
              "a dmx wildcard listener still admits a matching claim" );

      pKid.close();
      pSrv.close();
    }


    // -----------------------------------------------------------------
    // 21. ABI 6 -- past the messaging slice
    // -----------------------------------------------------------------
    // Ten methods that reach things the facade could always DO and never say:
    // drop one peer, run work and timers ON the pump thread, answer a message,
    // read a coded event, read and write per-connection knobs, probe a round
    // trip, and hand back the native hub. See missing.md.
    //
    // The through-line worth watching is the THREAD. Post, SetTimer and Ping
    // are called here, on the main thread, and their work happens on the hub's
    // pump -- each of them gets there by posting the hub a message under a
    // private topic, which is the same ordered path every other message takes.
    std::printf ( "\n-- ABI 6: past the messaging slice --\n" );

    {
      unsigned long   nMainThread = ::GetCurrentThreadId();
      std::atomic<unsigned long> nPostThread { 0 };
      std::atomic<unsigned int>  nPostKey    { 0 };
      void            *pPostCtx  = nullptr;
      int              nCtxValue = 4242;
      Flag             oPosted, oTimed, oDownEvt;
      std::atomic<unsigned int> nTimerId { 0 }, nTimerKey { 0 };

      p2pf::Hub extA = net.createHub ( L"Ext" );
      p2pf::Hub extB = net.createHub ( L"Ext.Node" );

      extA.onPost ( [&](unsigned int key, void *ctx)
      {
          nPostThread = ::GetCurrentThreadId();
          nPostKey    = key;
          pPostCtx    = ctx;
          oPosted.Set();
      } );
      extA.onTimer ( [&](unsigned int id, unsigned int key)
      {
          nTimerId = id; nTimerKey = key; oTimed.Set();
      } );
      extA.onPeerDown ( [&](const wchar_t*){ oDownEvt.Set(); } );

      // --- item 4: work onto the pump thread ---------------------------
      Check ( extA.post ( 77, &nCtxValue ) == S_OK && oPosted.Wait ( 5000 ),
              "post() runs OnPost on the hub, with its key and context intact" );
      Check ( nPostKey == 77 && pPostCtx == &nCtxValue,
              "...both arguments arrive unchanged" );
      Check ( nPostThread != 0 && nPostThread != nMainThread,
              "...and it ran on the PUMP thread, not the caller's" );

      // No extended sink, nowhere to deliver: refused at the call rather than
      // dropped later. (Only reachable through the raw ABI -- the Fn layer
      // always registers one.)
      extA.raw()->SetExtEvents ( nullptr );
      Check ( extA.post ( 1, nullptr ) == p2pf::P2PF_E_NO_SINK,
              "...post with no extended sink is P2PF_E_NO_SINK" );
      unsigned int uNoSink = 0;
      Check ( extA.setTimer ( 10, 1, &uNoSink ) == p2pf::P2PF_E_NO_SINK,
              "...so is a timer that would fire into nothing" );
      extA.raw()->SetExtEvents ( extA.rawSink() );

      // --- item 3: timers on the pump thread ---------------------------
      unsigned int uTimer = extA.setTimer ( 10, 0xBEEF );
      Check ( uTimer != 0 && oTimed.Wait ( 8000 ),
              "setTimer() fires OnTimer on the pump" );
      Check ( nTimerId == uTimer && nTimerKey == 0xBEEF,
              "...with the id it handed out and the key it was given" );

      unsigned int uLong = extA.setTimer ( 60000, 7 );
      Check ( uLong != 0 && extA.killTimer ( uLong ) == S_OK,
              "killTimer() accepts a live timer" );
      Check ( extA.killTimer ( 0xD00D ) == S_FALSE,
              "...and answers S_FALSE for an id it never issued" );

      // --- item 6: a round trip, and the per-connection knobs -----------
      Flag oExtUp;
      extB.onPeerUp ( [&](const wchar_t*){ oExtUp.Set(); } );
      Check ( net.link ( L"Ext", L"Ext.Node" ) == S_OK && oExtUp.Wait ( 10000 ),
              "an in-process edge for the rest of this section" );

      unsigned int uRtt = 0xFFFFFFFF;
      Check ( extB.ping ( L"Ext", 5000, &uRtt ) == S_OK && uRtt <= 5000,
              "ping() completes a facade-to-facade round trip" );
      std::printf ( "    (round trip %u ms)\n", uRtt );
      Check ( extB.ping ( L"Ext.Nobody", 400, &uRtt ) == p2pf::P2PF_E_TIMEOUT,
              "...and times out on an address nothing answers to" );

      unsigned int uMode = 0;
      Check ( extB.getConOption ( L"Ext", p2pf::P2PF_OPT_MODE, &uMode ) == S_OK &&
              uMode == p2pf::P2PF_CONMODE_DIAL,
              "getConOption reports the dialer's role from the kernel object" );
      unsigned int uMaxRecv = 0;
      Check ( extB.getConOption ( L"Ext", p2pf::P2PF_OPT_MAXRECV, &uMaxRecv ) == S_OK &&
              uMaxRecv > 0,
              "...and the connection's receive limit" );
      unsigned int uTrace = 99;
      Check ( extB.setConTrace ( L"Ext", true ) == S_OK &&
              extB.getConOption ( L"Ext", p2pf::P2PF_OPT_TRACE, &uTrace ) == S_OK &&
              uTrace == 1,
              "setConOption writes a knob the ABI could never reach before" );
      extB.setConTrace ( L"Ext", false );      // quiet again

      unsigned int uJunk = 0;
      Check ( extB.setConOption ( L"Ext", p2pf::P2PF_OPT_MODE, 1 ) == p2pf::P2PF_E_OPTION,
              "...a set on a read-only option is refused" );
      Check ( extB.getConOption ( L"Ext", 999, &uJunk ) == p2pf::P2PF_E_OPTION,
              "...and an option this build does not know" );
      Check ( extB.getConOption ( L"Ext.Ghost", p2pf::P2PF_OPT_MODE, &uJunk )
                == p2pf::P2PF_E_NO_PEER,
              "...and a peer this hub has no connection for" );

      // --- which connection a per-peer question is about ----------------
      //
      // A NAMED listen leaves TWO connections on the listening hub answering
      // to the same peer address: the service it armed, and the clone
      // AcceptSpawn posted when the peer arrived (P2PeerCon.cpp:221-256,
      // which copies the peer address across and hands the clone a Clone() of
      // the P2Peerio). The clone is the one that logs in and carries the
      // traffic; the service goes on waiting for the next peer.
      //
      // ConQuery returns the FIRST match, which is the service -- so every
      // option on a listening hub used to describe an idle object: no Login
      // bit, and a trace switched on there switched on nothing. The facade
      // now walks the hub's connections (EnumP2PmsgCon, exported from
      // Targetcore for this) and picks by what is being asked.
      unsigned int uStateL = 0, uStateD = 0;
      Check ( extA.getConOption ( L"Ext.Node", p2pf::P2PF_OPT_CONSTATE, &uStateL ) == S_OK &&
              extB.getConOption ( L"Ext",      p2pf::P2PF_OPT_CONSTATE, &uStateD ) == S_OK,
              "both ends of one edge report a connection state" );
      std::printf ( "    (listening end 0x%04X, dialing end 0x%04X)\n", uStateL, uStateD );
      Check ( ( uStateD & p2pf::P2PF_CONSTATE_LOGIN ) != 0,
              "...the DIALING end carries Login, as it always did" );
      Check ( ( uStateL & p2pf::P2PF_CONSTATE_LOGIN ) != 0,
              "...and so does the LISTENING end -- the session connection, "
              "not the idle service ConQuery would have found" );
      Check ( ( uStateL & ( p2pf::P2PF_CONSTATE_SEND | p2pf::P2PF_CONSTATE_RECV ) ) != 0,
              "...which is also the one doing the sending and receiving" );

      // MODE is the one option deliberately NOT answered from the session
      // connection: it reports what this hub DID, so it must agree with the
      // read side's P2PF_CON_LISTEN rather than say ACCEPTED on every named
      // listener.
      unsigned int uModeL = 0;
      Check ( extA.getConOption ( L"Ext.Node", p2pf::P2PF_OPT_MODE, &uModeL ) == S_OK &&
              uModeL == p2pf::P2PF_CONMODE_LISTEN,
              "...while MODE still reports what this hub armed" );

      // --- item 1: drop ONE peer, keep the hub -------------------------
      Check ( extB.disconnect ( L"Ext.Ghost" ) == p2pf::P2PF_E_NO_PEER,
              "disconnect() of an unknown peer says so" );
      Check ( extA.disconnect ( L"Ext.Node" ) == S_OK,
              "disconnect() drops one connection" );
      Check ( !extA.isPeerUp ( L"Ext.Node" ) && oDownEvt.Wait ( 3000 ),
              "...the peer is down and the client was told" );
      Check ( extA.address() != nullptr && extA.post ( 1, nullptr ) == S_OK,
              "...and the hub itself is untouched" );

      // --- item 6: the idle sweep --------------------------------------
      Check ( extA.closeIdleCons() == S_OK,
              "closeIdleCons() is accepted from a client thread" );

      // --- item 2: the escape hatch ------------------------------------
      void *pNative = extA.native();
      Check ( pNative != nullptr && pNative == extA.native(),
              "native() hands back the P2PeerHub, stably" );

      extB.close();
      extA.close();
    }

    // --- item 5: an answer, and a coded event ------------------------------
    // Two separate hubs so the filter cannot see the section above's traffic.
    {
      Flag oFiltered, oEvent;
      std::atomic<unsigned int> nCode { 0 };
      std::atomic<int>          nSeen { 0 };
      std::wstring              strPeer;

      p2pf::Hub fSrv = net.createHub ( L"Flt6.Server" );
      p2pf::Hub fCli = net.createHub ( L"Flt6.Client" );

      fSrv.onMessageEx ( [&](const p2pf::Message&) -> HRESULT
      {
          ++nSeen;
          oFiltered.Set();
          // Decline everything: the message travels on to the kernel's own
          // handlers, which is what "not mine" has to mean.
          return S_FALSE;
      } );
      // A sibling pair, so arming raises the unrelated-link report -- now
      // with a code and the peer it is about, not just a sentence.
      fCli.onEvent ( [&](unsigned int code, const wchar_t *peer, const wchar_t*)
      {
          nCode = code;
          strPeer = peer ? peer : L"";
          oEvent.Set();
      } );

      Check ( fSrv.listen ( L"Flt6.Client", L"dmx://P2PmsgExt6" )
                == p2pf::P2PF_S_UNRELATED_LINK,
              "a sibling arm still answers P2PF_S_UNRELATED_LINK" );
      Check ( fCli.connect ( L"Flt6.Server", L"dmx://P2PmsgExt6" )
                == p2pf::P2PF_S_UNRELATED_LINK && oEvent.Wait ( 3000 ),
              "...and the dialing side gets the report" );
      Check ( nCode == p2pf::P2PF_EVT_UNRELATED_LINK &&
              strPeer == L"Flt6.Server",
              "...as a CODE and a peer, which is what a client can branch on" );

      Flag oUp;
      fCli.onPeerUp ( [&](const wchar_t*){ oUp.Set(); } );
      if ( oUp.Wait ( 10000 ) )
      {
        Check ( fCli.sendText ( L"Flt6.Server", L"declined", L"x" ) == S_OK &&
                oFiltered.Wait ( 5000 ) && nSeen > 0,
                "onMessageEx sees the message and may decline it" );
      }
      else
        Check ( false, "onMessageEx: the sibling link never came up" );

      fCli.close();
      fSrv.close();
    }

    // -----------------------------------------------------------------
    // 22. The boundaries of those ten
    // -----------------------------------------------------------------
    // Three things ABI 6 shipped as recorded limitations rather than as
    // behaviour. Each is now a promise, and this is where the promise is
    // measured -- which for two of the three means reproducing the thing that
    // used to go wrong, not just observing that it does not.
    std::printf ( "\n-- ABI 6: the boundaries --\n" );

    // --- a timer never fires EARLY -----------------------------------
    //
    // The kernel builds a timer's deadline from `_time64(0)*1000` and polls it
    // against the same one-second clock (P2Pwin32.cpp:5321 and :515). The two
    // truncations cancel while the pump's next wake-up is the timer's OWN
    // sleep -- which is why a quiet hub always looked right -- and stop
    // cancelling the moment anything else wakes it first: that wake-up reads a
    // deadline truncated into the past and fires the timer there and then.
    //
    // So this check needs BOTH forcing functions. ParkBeforeSecondBoundary
    // puts the arm ~5 ms before a boundary, and the post() loop keeps the pump
    // waking for other reasons across it. Measured without the facade's own
    // deadline: this 200 ms timer fired in 5 ms.
    //
    // What is asserted is the ONE-SIDED promise: never early. Late is still
    // possible by up to about a second, because when the pump notices an
    // elapsed timer is the kernel's granularity and not something the facade
    // can reach from outside.
    {
      const unsigned int kAskedFor = 200;

      Flag oEdgeTimer;
      std::atomic<unsigned long long> uFiredAt { 0 };

      p2pf::Hub tHub = net.createHub ( L"Edge.Timer" );
      tHub.onTimer ( [&](unsigned int, unsigned int)
                     { uFiredAt = ::GetTickCount64(); oEdgeTimer.Set(); } );

      ParkBeforeSecondBoundary ( 5 );
      unsigned long long uArmedAt = ::GetTickCount64();
      unsigned int       uEdgeId  = tHub.setTimer ( kAskedFor, 0xE0 );

      // Traffic, so the pump has a reason to wake that is not this timer.
      for ( int nPoke = 0; nPoke < 200 && !oEdgeTimer.m_bSet; ++nPoke )
      {
        tHub.post ( 0, nullptr );
        ::Sleep ( 5 );
      }

      bool  bFired   = ( uEdgeId != 0 ) && oEdgeTimer.Wait ( 8000 );
      long  nElapsed = bFired ? (long)( uFiredAt - uArmedAt ) : -1;

      std::printf ( "    (asked for %u ms, waited %ld ms)\n", kAskedFor, nElapsed );
      // 50 ms of slack for GetTickCount64's own ~16 ms granularity at both
      // ends -- far tighter than the ~200 ms this is telling apart from 0.
      Check ( bFired && nElapsed >= (long)kAskedFor - 50,
              "a timer armed against a second boundary does NOT fire early" );
      Check ( bFired && nElapsed < 3000,
              "...and is late by the kernel's granularity, not by more" );

      tHub.close();
    }

    // --- Close() outlives no ping ------------------------------------
    //
    // A Ping parks a CLIENT thread on an event owned by the hub, and Close()
    // ends in `delete this`. Releasing the waiter was never enough: the close
    // has to know the waiter has LEFT before it frees the object underneath
    // it, and it now waits for exactly that.
    //
    // BE CLEAR ABOUT WHAT THIS PROVES. The handshake itself is not observable
    // from out here -- the free either races or it does not, and the teardown
    // that follows the release is long enough that it usually did not, which
    // is why this was a documented hazard rather than a reported crash. What
    // is asserted is the contract a caller can hold the facade to: a close
    // with a ping outstanding comes back at once rather than sitting out the
    // budget, and the ping reports failure rather than an invented answer.
    // Several rounds because it is a race, and one round proves nothing.
    {
      const int kPingCloseRounds = 3;

      int nPrompt = 0, nFailed = 0;
      for ( int nRound = 0; nRound < kPingCloseRounds; ++nRound )
      {
        std::wstring strAddr = L"Edge.Ping";
        strAddr += (wchar_t)( L'0' + nRound );

        p2pf::Hub        pHub = net.createHub ( strAddr.c_str() );
        p2pf::IP2PHub   *pRaw = pHub.raw();     // captured: close() clears it
        std::atomic<HRESULT> hrPing { S_OK };

        // A budget nothing will answer inside, so the waiter is still parked
        // when the close arrives.
        std::thread thPing ( [&]
        {
            unsigned int uMs = 0;
            hrPing = pRaw->Ping ( L"Edge.Nobody", 20000, &uMs );
        } );
        ::Sleep ( 300 );

        unsigned long long uAt = ::GetTickCount64();
        pHub.close();
        unsigned long long uTook = ::GetTickCount64() - uAt;
        thPing.join();

        if ( uTook < 5000 )              ++nPrompt;
        if ( FAILED ( (HRESULT)hrPing ) ) ++nFailed;
      }

      Check ( nPrompt == kPingCloseRounds,
              "close() with a ping outstanding returns at once, every round" );
      Check ( nFailed == kPingCloseRounds,
              "...and the ping comes back failed rather than answered" );
    }

    // --- a decline is reported to the SENDER, on every path -----------
    //
    // S_FALSE from OnMessageEx means "not mine". On a topic-named unicast the
    // facade honours it by letting the message travel on, and the kernel's own
    // NotHandled then posts an exception back to the sender -- the half that
    // was never proved here. On a BROADCAST the base handler is the relay and
    // must run whatever the client says, so the answer used to be read and
    // discarded; now the same report is sent from the facade after the relay.
    // The third check is the one that keeps the other two honest.
    {
      Flag oDeclined, oTaken, oRouteErr, oDecUp;
      std::atomic<int> nRouteErr { 0 }, nSrvEvent { 0 };

      p2pf::Hub dSrv = net.createHub ( L"Dec" );
      p2pf::Hub dCli = net.createHub ( L"Dec.Node" );

      dSrv.onMessageEx ( [&](const p2pf::Message& m) -> HRESULT
      {
          if ( m.topic && ::wcscmp ( m.topic, L"taken" ) == 0 )
          {
            oTaken.Set();
            return S_OK;
          }
          oDeclined.Set();
          return S_FALSE;
      } );
      dCli.onEvent ( [&](unsigned int code, const wchar_t*, const wchar_t*)
      {
          if ( code == p2pf::P2PF_EVT_ROUTING_ERROR )
          {
            ++nRouteErr;
            oRouteErr.Set();
          }
      } );
      dCli.onPeerUp ( [&](const wchar_t*){ oDecUp.Set(); } );
      // The DECLINER must hear nothing: it sent no message, so no bounce is
      // about it. (The kernel does bounce an unclaimed exception one hop
      // further, back this way -- the facade reports a bounce only when what
      // bounced is not itself one.)
      dSrv.onEvent ( [&](unsigned int, const wchar_t*, const wchar_t*)
                     { ++nSrvEvent; } );

      Check ( net.link ( L"Dec", L"Dec.Node" ) == S_OK && oDecUp.Wait ( 10000 ),
              "an in-process edge for the decline checks" );

      Check ( dCli.sendText ( L"Dec", L"nobody-wants-this", L"x" ) == S_OK &&
              oDeclined.Wait ( 5000 ) && oRouteErr.Wait ( 5000 ),
              "a declined UNICAST comes back to the sender as a routing error" );

      oDeclined.Reset();
      oRouteErr.Reset();
      int nBefore = nRouteErr;
      Check ( dCli.broadcast ( L"nobody-wants-this", "x", 1 ) == S_OK &&
              oDeclined.Wait ( 5000 ) && oRouteErr.Wait ( 5000 ) &&
              nRouteErr > nBefore,
              "...and so does a declined BROADCAST, which used to say nothing" );

      oRouteErr.Reset();
      nBefore = nRouteErr;
      Check ( dCli.sendText ( L"Dec", L"taken", L"x" ) == S_OK &&
              oTaken.Wait ( 5000 ),
              "a message the sink ACCEPTS is delivered as it always was" );
      ::Sleep ( 1500 );
      Check ( nRouteErr == nBefore,
              "...and reports nothing back: only a decline does" );
      Check ( nSrvEvent == 0,
              "...and the hub that DECLINED was told nothing either way" );

      dCli.close();
      dSrv.close();
    }

    // -----------------------------------------------------------------
    // 23. ABI 7 -- the message model
    //
    // What this section is really for: every one of these properties is
    // carried in P2PeerMsgPrefix, which lives inside the message's data
    // blob, and NOTHING in the kernel or its documentation promises that
    // blob is transmitted field-for-field. The whole feature rests on it,
    // so the tag is proved over a REAL TCP CONNECTION rather than the
    // in-process Dmx edge the rest of this suite reaches for -- Dmx hands
    // the object across and would prove nothing about serialisation.
    // -----------------------------------------------------------------
    {
      std::printf ( "\n-- ABI 7: priority, correlation, destination --\n" );

      Flag oMsgUp, oReq, oAnsA, oAnsB, oPri, oBCast, oDefault;
      unsigned int uSeenTag = 0, uSeenPri = 0, uSeenFlags = 0;
      unsigned int uAnsTagA = 0, uAnsTagB = 0, uBCastTag = 0;
      unsigned int uDefaultTag = 0, uDefaultFlags = 0;
      std::wstring strSeenDest, strBCastDest;
      HRESULT      hrOutside = S_OK;

      p2pf::Hub mSrv = net.createHub ( L"Msg" );
      p2pf::Hub mCli = net.createHub ( L"Msg.Node" );

      // The responder: answers with reply(), which is the whole of
      // request/response on this ABI -- back to the sender, carrying the
      // tag it arrived with, and nothing of either in the payload.
      mSrv.onTopic ( L"req", [&](const p2pf::Message& m)
                     {
                       uSeenTag    = m.tag;
                       uSeenPri    = m.priority;
                       uSeenFlags  = m.flags;
                       strSeenDest = m.destination();
                       mSrv.replyText ( m, L"ans", L"ok" );
                       oReq.Set();
                     } );
      mSrv.onTopic ( L"pri", [&](const p2pf::Message& m)
                     { uSeenPri = m.priority; oPri.Set(); } );
      mSrv.onTopic ( L"plain", [&](const p2pf::Message& m)
                     {
                       uDefaultTag   = m.tag;
                       uDefaultFlags = m.flags;
                       oDefault.Set();
                     } );
      // GetMsgInfo is defined INSIDE a delivery and nowhere else. Asking
      // from a peer-lifecycle callback is the cheapest way to prove the
      // gate does something, since it runs on the very same pump thread --
      // a thread check alone would pass it.
      mSrv.onPeerUp ( [&](const wchar_t*)
                      {
                        hrOutside = mSrv.raw()->GetMsgInfo ( nullptr, nullptr,
                                                             nullptr, nullptr,
                                                             nullptr );
                        oMsgUp.Set();
                      } );

      mCli.onTopic ( L"ans", [&](const p2pf::Message& m)
                     {
                       if ( m.tag == 0x1234 ) { uAnsTagA = m.tag; oAnsA.Set(); }
                       if ( m.tag == 0x5678 ) { uAnsTagB = m.tag; oAnsB.Set(); }
                     } );
      mCli.onTopic ( L"sweep", [&](const p2pf::Message& m)
                     {
                       uBCastTag    = m.tag;
                       strBCastDest = m.destination();
                       oBCast.Set();
                     } );

      Check ( mSrv.listen  ( L"Msg.Node", L"tcp://:7823" ) == S_OK &&
              mCli.connect ( L"Msg",      L"tcp://127.0.0.1:7823" ) == S_OK &&
              oMsgUp.Wait ( 10000 ),
              "a REAL TCP edge for the message-model checks" );

      Check ( hrOutside == p2pf::P2PF_E_NO_MESSAGE,
              "GetMsgInfo outside a delivery is P2PF_E_NO_MESSAGE, not zeroes" );

      // The load-bearing measurement.
      Check ( mCli.sendTextEx ( L"Msg", L"req", L"?", 0x1234 ) == S_OK &&
              oReq.Wait ( 5000 ) && uSeenTag == 0x1234,
              "a tag survives a real TCP wire intact" );

      Check ( strSeenDest == L"Msg",
              "the destination reads as the addressee -- measured, not assumed" );

      Check ( ( uSeenFlags & p2pf::P2PF_MSG_BOUNCES   ) != 0 &&
              ( uSeenFlags & p2pf::P2PF_MSG_BROADCAST ) == 0,
              "a plain unicast reports bounces-on and not-broadcast" );

      // Two requests in flight at once, answered in whatever order the
      // pump gets to them: the case that was NOT expressible before, and
      // the reason missing.md §4.2 called this the sharpest consequence.
      Check ( mCli.sendTextEx ( L"Msg", L"req", L"?", 0x1234 ) == S_OK &&
              mCli.sendTextEx ( L"Msg", L"req", L"?", 0x5678 ) == S_OK &&
              oAnsA.Wait ( 5000 ) && oAnsB.Wait ( 5000 ) &&
              uAnsTagA == 0x1234 && uAnsTagB == 0x5678,
              "two overlapping requests are told apart by tag alone" );

      Check ( mCli.sendTextEx ( L"Msg", L"pri", L"x", 0,
                                p2pf::P2PF_PRI_HIGH ) == S_OK &&
              oPri.Wait ( 5000 ) && uSeenPri == p2pf::P2PF_PRI_HIGH,
              "a priority round-trips as the kernel's own value" );

      Check ( mSrv.broadcastEx ( L"sweep", "s", 1, 0x900D ) == S_OK &&
              oBCast.Wait ( 5000 ) && uBCastTag == 0x900D,
              "a broadcast carries its tag to every receiver" );

      Check ( strBCastDest == L"Msg.Node",
              "...and a broadcast's destination is the RECEIVER: the kernel "
              "redirects each copy, so it is a last-hop fact" );

      Check ( mCli.sendText ( L"Msg", L"plain", L"x" ) == S_OK &&
              oDefault.Wait ( 5000 ) && uDefaultTag == 0 &&
              ( uDefaultFlags & p2pf::P2PF_MSG_BOUNCES ) != 0,
              "a plain Send still arrives with no tag and bounces on" );

      mCli.close();
      mSrv.close();
    }

    // -----------------------------------------------------------------
    // 24. P2PF_SEND_NO_BOUNCE
    //
    // The kernel's own per-message exception toggle -- P2PeerMsg::
    // Exceptions(), which four comments in P2PeerTarget.cpp point at -- is
    // DECLARED AND IMPLEMENTED NOWHERE, and nothing reads the control byte
    // it lives in. So this flag is the facade's, and the pair of checks
    // below is what makes it a behaviour rather than a claim: the same
    // decline, with and without it.
    // -----------------------------------------------------------------
    {
      std::printf ( "\n-- ABI 7: fire and forget --\n" );

      Flag oNbUp, oDeclined, oRouteErr;
      std::atomic<int> nRouteErr{0};

      p2pf::Hub nSrv = net.createHub ( L"Nb" );
      p2pf::Hub nCli = net.createHub ( L"Nb.Node" );

      nSrv.onMessageEx ( [&](const p2pf::Message&) -> HRESULT
                         { oDeclined.Set(); return S_FALSE; } );
      nCli.onEvent ( [&](unsigned int code, const wchar_t*, const wchar_t*)
                     {
                       if ( code == p2pf::P2PF_EVT_ROUTING_ERROR )
                         { ++nRouteErr; oRouteErr.Set(); }
                     } );
      nCli.onPeerUp ( [&](const wchar_t*){ oNbUp.Set(); } );

      Check ( net.link ( L"Nb", L"Nb.Node" ) == S_OK && oNbUp.Wait ( 10000 ),
              "an in-process edge for the no-bounce checks" );

      // The control. Without the flag, §5.4's report still happens --
      // which is what keeps the check below from passing for the wrong
      // reason (a decline that never arrived would look identical).
      Check ( nCli.sendText ( L"Nb", L"unwanted", L"x" ) == S_OK &&
              oDeclined.Wait ( 5000 ) && oRouteErr.Wait ( 5000 ),
              "a declined message still reports back when nothing asks it not to" );

      oDeclined.Reset();
      int nBefore = nRouteErr;
      Check ( nCli.sendTextEx ( L"Nb", L"unwanted", L"x", 0,
                                p2pf::P2PF_PRI_DEFAULT,
                                p2pf::P2PF_SEND_NO_BOUNCE ) == S_OK &&
              oDeclined.Wait ( 5000 ),
              "a NO_BOUNCE message is delivered and declined exactly as before" );
      ::Sleep ( 1500 );
      Check ( nRouteErr == nBefore,
              "...and the sender is told NOTHING -- the report is not generated" );

      // Same suppression on the broadcast path, which reaches it through
      // ReportDeclined rather than the handler chain.
      oDeclined.Reset();
      nBefore = nRouteErr;
      Check ( nCli.broadcastEx ( L"unwanted", "x", 1, 0,
                                 p2pf::P2PF_PRI_DEFAULT,
                                 p2pf::P2PF_SEND_NO_BOUNCE ) == S_OK &&
              oDeclined.Wait ( 5000 ),
              "a NO_BOUNCE BROADCAST is declined the same way" );
      ::Sleep ( 1500 );
      Check ( nRouteErr == nBefore,
              "...and says nothing either: one flag, both paths" );

      nCli.close();
      nSrv.close();
    }

    // -----------------------------------------------------------------
    // 25. ABI 8 -- named fields
    //
    // The question this section exists to answer is whether the kernel's
    // message TREE crosses a wire, not just its prefix. Fields ride as a
    // child of the message root, beside the kernel's own Net/Msg/Wrp/Evt
    // slots, and the argument that they travel is "PrepareP2Piomage ships
    // the whole VBList when the IFmask is all-ones, and it is". That is an
    // argument. TCP is the fact.
    //
    // The second question is compatibility: the payload must be exactly
    // where it has always been, so that a peer knowing nothing of fields
    // reads what it always read.
    // -----------------------------------------------------------------
    {
      std::printf ( "\n-- ABI 8: named fields --\n" );

      Flag oFldUp, oGot, oPlain, oEmptyGot;
      std::wstring strWho, strMissing = L"unset";
      std::vector<unsigned char> aRaw;
      std::vector<std::wstring>  aNames;
      unsigned int uFlagsSeen = 0, uFieldCount = 0;
      bool bHadEmpty = false, bHadAbsent = false;
      std::string strPlainBody;
      unsigned int uPlainFields = 99;

      p2pf::Hub fSrv = net.createHub ( L"Fld" );
      p2pf::Hub fCli = net.createHub ( L"Fld.Node" );

      fSrv.onTopic ( L"rec", [&](const p2pf::Message& m)
                     {
                       uFlagsSeen = m.flags;
                       strWho     = m.fieldText ( L"who" );
                       aRaw       = m.field     ( L"raw" );
                       aNames     = m.fieldNames();
                       uFieldCount = (unsigned int)aNames.size();
                       // present-and-empty vs absent: the distinction the
                       // ABI keeps deliberately, and the only way a client
                       // can use presence as a signal.
                       bHadEmpty  = m.has ( L"flag" ) && m.field ( L"flag" ).empty();
                       bHadAbsent = !m.has ( L"nope" );
                       // The payload must be untouched by all of this.
                       const char *p = (const char*)m.payload;
                       strPlainBody.assign ( p, p + m.size );
                       oGot.Set();
                     } );
      // A message with NO fields, to prove the readers answer "none" rather
      // than failing, and that P2PF_MSG_FIELDS is not set.
      fSrv.onTopic ( L"bare", [&](const p2pf::Message& m)
                     {
                       uPlainFields = (unsigned int)m.fieldNames().size()
                                    + ( m.hasFields() ? 100 : 0 );
                       oPlain.Set();
                     } );
      fSrv.onPeerUp ( [&](const wchar_t*){ oFldUp.Set(); } );

      Check ( fSrv.listen  ( L"Fld.Node", L"tcp://:7824" ) == S_OK &&
              fCli.connect ( L"Fld",      L"tcp://127.0.0.1:7824" ) == S_OK &&
              oFldUp.Wait ( 10000 ),
              "a REAL TCP edge for the field checks" );

      {
        p2pf::OutMessage msg = net.createMessage();
        const unsigned char kRaw[] = { 0x00, 0xFF, 0x10, 0x00, 0x7F };
        Check ( msg.setText ( L"who", L"node-7" ) == S_OK &&
                msg.set     ( L"raw", kRaw, sizeof(kRaw) ) == S_OK &&
                msg.set     ( L"flag", nullptr, 0 ) == S_OK &&
                msg.payload ( "BODY", 4 ) == S_OK &&
                msg.count() == 3,
                "a message takes a payload and three fields" );

        Check ( fCli.sendMsg ( L"Fld", L"rec", msg ) == S_OK &&
                oGot.Wait ( 5000 ),
                "...and crosses a real TCP wire" );
      }

      Check ( strWho == L"node-7",
              "a text field arrives intact" );
      Check ( aRaw.size() == 5 && aRaw[0] == 0x00 && aRaw[1] == 0xFF &&
              aRaw[3] == 0x00 && aRaw[4] == 0x7F,
              "...and a BINARY one does, embedded NULs and all" );
      Check ( strPlainBody == "BODY",
              "...while the PAYLOAD is exactly where it always was" );
      Check ( ( uFlagsSeen & p2pf::P2PF_MSG_FIELDS ) != 0,
              "P2PF_MSG_FIELDS says asking is worth it" );
      Check ( bHadEmpty,
              "a field that is PRESENT AND EMPTY reads as present" );
      Check ( bHadAbsent,
              "...and an absent one reads as absent: different answers" );
      Check ( uFieldCount == 3 && aNames.size() == 3 &&
              aNames[0] == L"who" && aNames[1] == L"raw" && aNames[2] == L"flag",
              "the names enumerate in the order the sender set them" );

      Check ( fCli.sendText ( L"Fld", L"bare", L"x" ) == S_OK &&
              oPlain.Wait ( 5000 ) && uPlainFields == 0,
              "a message with no fields reports none, and does not claim any" );

      // The refusals.
      {
        p2pf::OutMessage msg = net.createMessage();
        Check ( msg.setText ( L"P2PFsneaky", L"x" ) == p2pf::P2PF_E_RESERVED_TOPIC,
                "a field name in the facade's own namespace is refused" );

        std::wstring strLong ( p2pf::MAX_FIELD_NAME + 1, L'n' );
        Check ( msg.setText ( strLong.c_str(), L"x" ) == p2pf::P2PF_E_FIELD_LIMIT,
                "...so is a name past MAX_FIELD_NAME" );

        std::vector<char> aBig ( p2pf::MAX_FIELD_SIZE + 1, 'x' );
        Check ( msg.set ( L"big", &aBig[0], (unsigned int)aBig.size() )
                  == p2pf::P2PF_E_FIELD_LIMIT,
                "...and a value past MAX_FIELD_SIZE" );

        // The one that a per-field cap alone would miss: many legal fields
        // adding up to one message the kernel would drop the connection over.
        std::vector<char> aChunk ( 4096, 'y' );
        HRESULT hrTotal = S_OK;
        for ( int i = 0; i < 12 && SUCCEEDED(hrTotal); ++i )
        {
          wchar_t wszName[32];
          std::swprintf ( wszName, 32, L"chunk%d", i );
          msg.set ( wszName, &aChunk[0], (unsigned int)aChunk.size() );
          hrTotal = fCli.sendMsg ( L"Fld", L"rec", msg );
        }
        Check ( hrTotal == E_INVALIDARG,
                "twelve legal fields that overflow one message are refused AT SEND" );
      }

      // A field read outside a delivery, like every other current-message
      // reader in this ABI.
      unsigned int cbOut = 0;
      Check ( fSrv.raw()->GetField ( L"who", nullptr, &cbOut )
                == p2pf::P2PF_E_NO_MESSAGE,
              "GetField outside a delivery is P2PF_E_NO_MESSAGE" );

      fCli.close();
      fSrv.close();
    }

    // -----------------------------------------------------------------
    // 26. ABI 9 -- the client's own thread runs the pump
    //
    // Every other section in this file talks to a hub that owns a thread.
    // These hubs own none: they were created on THIS thread and do nothing
    // whatever until this thread asks them to. So the checks below cannot
    // use Flag::Wait -- sleeping is precisely the thing that stops a
    // caller-pumped hub working, and a test that slept would fail for the
    // right reason and teach nothing. pumpFor() is the stand-in, and using
    // it everywhere is itself the point being made.
    //
    // Over a REAL TCP connection rather than the in-process Dmx edge: two
    // caller-pumped hubs on one thread would need two pumps on one thread,
    // which the kernel forbids -- so one end is caller-pumped and the other
    // is an ordinary spawned hub, which is also the deployment that will
    // actually happen (a GUI talking to a service).
    // -----------------------------------------------------------------
    {
      std::printf ( "\n-- ABI 9: caller-driven pumping --\n" );

      const DWORD dwMain = ::GetCurrentThreadId();

      // The far end: an ordinary hub, spawned, exactly as every other
      // section makes one. It answers whatever it is asked.
      p2pf::Hub pSrv = net.createHub ( L"Pmp" );
      pSrv.onTopic ( L"ask", [&](const p2pf::Message& m)
                     { pSrv.replyText ( m, L"tell", L"here" ); } );

      // The hub under test.
      p2pf::Hub pCli = net.createHub ( L"Pmp.Ui",
                                       p2pf::P2PF_HUB_CALLER_PUMPED );

      Check ( pCli.callerPumped() && !pSrv.callerPumped(),
              "GetPumpInfo tells the two kinds of hub apart" );
      Check ( pCli.onPumpThread() && !pSrv.onPumpThread(),
              "...and says the CALLER owns the caller-pumped one" );
      {
        unsigned int uFlags = 0, uTid = 0;
        pCli.raw()->GetPumpInfo ( &uFlags, &uTid );
        Check ( uTid == (unsigned int)dwMain,
                "...naming this very thread as the one that runs it" );
      }

      Check ( pSrv.raw()->Pump ( 0, nullptr ) == p2pf::P2PF_E_NOT_PUMPED,
              "Pump on a hub that owns a thread is P2PF_E_NOT_PUMPED" );

      // Nothing happens until we pump. The connection is armed on both
      // sides here and the caller-pumped end is then deliberately left
      // alone: a spawned hub would be logged in within milliseconds.
      DWORD dwUpThread = 0;
      Flag  oUp;
      pCli.onPeerUp ( [&](const wchar_t*)
                      { dwUpThread = ::GetCurrentThreadId(); oUp.Set(); } );

      Check ( Armed ( pSrv.listen  ( L"Pmp.Ui", L"tcp://:7826" ) ) &&
              Armed ( pCli.connect ( L"Pmp",    L"tcp://127.0.0.1:7826" ) ),
              "a caller-pumped hub arms connections like any other" );

      ::Sleep ( 400 );
      Check ( !oUp.Wait ( 0 ),
              "...and then does NOTHING AT ALL until it is pumped" );

      // Now run it. This is the whole of a caller-pumped main loop.
      pCli.run ( [&]{ return oUp.Wait ( 0 ); }, 25 );

      Check ( oUp.Wait ( 0 ),
              "one client-driven loop is enough to bring the peer up" );
      Check ( dwUpThread == dwMain,
              "...and the callback ran on the CALLER's thread -- the property"
              " this whole shape exists for" );

      // A round trip, driven entirely from here.
      Flag oTell;
      DWORD dwMsgThread = 0;
      unsigned int uTag = 0;
      pCli.onTopic ( L"tell", [&](const p2pf::Message& m)
                     {
                       dwMsgThread = ::GetCurrentThreadId();
                       uTag        = m.tag;
                       oTell.Set();
                     } );

      Check ( pCli.sendTextEx ( L"Pmp", L"ask", L"?", 0x99 ) == S_OK,
              "a caller-pumped hub sends from its own thread" );
      pCli.run ( [&]{ return oTell.Wait ( 0 ); }, 25 );
      Check ( oTell.Wait ( 0 ) && dwMsgThread == dwMain && uTag == 0x99,
              "...and the answer is delivered on that same thread, tag intact" );

      // Pump(0) must not park: that is what makes it usable from a GUI idle
      // handler. Measured on an idle hub, where a blocking call would sit out
      // its whole budget.
      {
        ULONGLONG uT0 = ::GetTickCount64();
        for ( int i = 0; i < 20; ++i )
          pCli.raw()->Pump ( 0, nullptr );
        ULONGLONG uElapsed = ::GetTickCount64() - uT0;
        Check ( uElapsed < 100,
                "Pump(0) on an idle hub returns AT ONCE, 20 times over" );
      }
      {
        unsigned int uWhat = 99;
        HRESULT hrIdle = pCli.raw()->Pump ( 0, &uWhat );
        Check ( hrIdle == S_FALSE && uWhat == p2pf::P2PF_PUMP_NOTHING,
                "...answering S_FALSE and P2PF_PUMP_NOTHING for 'idle'" );
      }

      // GetPending is answerable on both kinds and from any thread.
      {
        unsigned int uNone = 0xFFFF;
        Check ( SUCCEEDED ( pCli.raw()->GetPending ( &uNone ) ) &&
                SUCCEEDED ( pSrv.raw()->GetPending ( &uNone ) ),
                "GetPending answers for a spawned hub too" );
      }

      // THE THING THIS BUYS BACK. On a spawned hub these two refuse from the
      // pump thread; here the caller IS the pump, so the wait drives it.
      {
        unsigned int uMs = 0;
        HRESULT hrPing = pCli.raw()->Ping ( L"Pmp", 5000, &uMs );
        Check ( hrPing == S_OK,
                "Ping works from the thread that owns the pump" );
      }
      // ...including from INSIDE a callback, which is the case the whole
      // P2PF_E_PUMP_THREAD rule exists to forbid on a spawned hub. A TIMER
      // callback, because a timer is dispatched by the pump directly (the
      // Step 1 block of PumpP2Pmsg) and so re-entering the pump from one
      // re-enters nothing else.
      {
        HRESULT hrNested = E_FAIL;
        Flag    oNested;
        unsigned int uTimerId = 0;
        pCli.onTimer ( [&](unsigned int, unsigned int)
                       {
                         unsigned int ms = 0;
                         hrNested = pCli.raw()->Ping ( L"Pmp", 5000, &ms );
                         oNested.Set();
                       } );
        pCli.setTimer ( 20, 1, &uTimerId );
        pCli.run ( [&]{ return oNested.Wait ( 0 ); }, 25 );
        Check ( hrNested == S_OK,
                "...and from inside a TIMER callback, which a spawned hub"
                " forbids outright" );
      }

      // ...but NOT from inside the delivery of a message that arrived on the
      // very connection being pinged, and this is a real boundary rather than
      // a missing feature. The pump is re-entered happily enough; what does
      // not happen is any further RECEIVE on that connection, because the
      // frame currently being dispatched is what its completion handler is in
      // the middle of. So the answer cannot arrive until the outer callback
      // returns, and the ping spends its whole budget. Measured, not assumed
      // -- and worth a check of its own so that a later change which makes it
      // work does not do so silently.
      {
        HRESULT hrSameCon = E_FAIL;
        Flag    oSameCon;
        pCli.onTopic ( L"tell", [&](const p2pf::Message&)
                       {
                         unsigned int ms = 0;
                         hrSameCon = pCli.raw()->Ping ( L"Pmp", 300, &ms );
                         oSameCon.Set();
                       } );
        pCli.sendText ( L"Pmp", L"ask", L"?" );
        pCli.run ( [&]{ return oSameCon.Wait ( 0 ); }, 25 );
        Check ( hrSameCon == p2pf::P2PF_E_TIMEOUT,
                "...though not while inside a delivery FROM the peer being"
                " pinged: that connection cannot receive again until the"
                " handler returns" );
      }
      // The control: the same call on the SPAWNED hub still refuses, so the
      // check above cannot be passing because the guard went away.
      {
        HRESULT hrGuard = E_FAIL;
        Flag    oGuard;
        pSrv.onTopic ( L"guard", [&](const p2pf::Message&)
                       {
                         unsigned int ms = 0;
                         hrGuard = pSrv.raw()->Ping ( L"Pmp.Ui", 500, &ms );
                         oGuard.Set();
                       } );
        pCli.sendText ( L"Pmp", L"guard", L"x" );
        pCli.pumpFor ( 500 );
        oGuard.Wait ( 2000 );
        Check ( hrGuard == p2pf::P2PF_E_PUMP_THREAD,
                "a SPAWNED hub still refuses Ping from its pump thread" );
      }

      // Wrong thread: everything that waits must come from the owner.
      {
        HRESULT hrPumpOther  = S_OK;
        HRESULT hrCloseOther = S_OK;
        std::thread oOther ( [&]
        {
          hrPumpOther  = pCli.raw()->Pump ( 0, nullptr );
          hrCloseOther = pCli.raw()->Close ( );
        } );
        oOther.join();
        Check ( hrPumpOther == p2pf::P2PF_E_PUMP_OWNER,
                "Pump from another thread is P2PF_E_PUMP_OWNER" );
        Check ( hrCloseOther == p2pf::P2PF_E_PUMP_OWNER,
                "...so is Close, and the hub is still usable after it" );
      }
      Check ( pCli.raw()->Pump ( 0, nullptr ) != p2pf::P2PF_E_CLOSED,
              "...which the refused Close proves by leaving the hub alive" );

      // Disconnect, from the owning thread, on a hub with a live peer.
      Check ( pCli.raw()->Disconnect ( L"Pmp" ) == S_OK,
              "Disconnect works from the thread that owns the pump" );

      // Close from the owning thread, and then the hub is gone for good.
      pCli.close();
      pSrv.close();
      Check ( true, "a caller-pumped hub closes on its owning thread" );
    }

    // -----------------------------------------------------------------
    // 27. ABI 10 -- the kernel narrates, and now something can hear it
    // -----------------------------------------------------------------
    // Everything else in this suite is about what the facade DOES. This is
    // about what the kernel SAYS while doing it -- until now, into a void.
    //
    // The property under test that is not like anything else here: this is the
    // ONE callback in the ABI that is not delivered on a pump thread. The
    // kernel has exactly one static notification slot and calls it inline, as
    // the event is disposed of, on whatever thread raised it. So the check
    // that matters most below is a thread-id comparison, twice -- once from
    // this thread and once from a pump -- because if that ever stopped being
    // true, every handler written against this contract would silently start
    // needing a lock it does not have.
    std::printf ( "\n-- ABI 10: the kernel narrates --\n" );

    {
      const unsigned long nMainThread = ::GetCurrentThreadId();

      // Captured under a mutex, not because this test is contended but because
      // the contract says the handler can run on several threads at once and a
      // test that ignored that would be the wrong example to copy.
      std::mutex   oCap;
      std::wstring strMod, strText, strClass, strAdvice;
      std::atomic<unsigned int> nSeverity { 0 }, nEventNo { 0 };
      std::atomic<unsigned int> nThread   { 0 }, nTime    { 0 };
      std::atomic<int>          nMine     { 0 }, nOther   { 0 };
      std::atomic<int>          nBadPart  { 0 };
      HRESULT hrInsideRaise = S_OK;
      HRESULT hrBadPart     = S_OK;

      // "P2PFTest:" marks the lines this section raised, so the kernel's own
      // narration -- which arrives on the same stream and is the point of the
      // exercise -- cannot be mistaken for them, or they for it.
      auto oHandler = [&]( const p2pf::DiagEvent& d )
      {
          if ( ::wcsncmp ( d.text(), L"P2PFTest:", 9 ) != 0 )
          {
            ++nOther;
            return;
          }
          ++nMine;
          nSeverity = d.severity();
          nEventNo  = d.eventNo();
          nThread   = d.threadId();
          nTime     = d.time();
          {
            std::lock_guard<std::mutex> lk ( oCap );
            strMod    = d.module();
            strText   = d.text();
            strClass  = d.className();
            strAdvice = d.advice();
          }
          // Both of the rules that only hold INSIDE a delivery, asked from
          // the one place they hold.
          if ( nBadPart == 0 )
          {
            ++nBadPart;
            unsigned int cch = 0;
            hrBadPart = net.raw()->GetDiagText ( 999, nullptr, &cch );
          }
      };

      Check ( net.onDiag ( oHandler, p2pf::P2PF_DIAGM_APP ) == S_OK,
              "a diagnostics sink registers on the network" );
      Check ( net.diagMask() == p2pf::P2PF_DIAGM_APP,
              "...and the mask it was given reads back" );
      Check (  net.diagWanted ( p2pf::P2PF_DIAGM_APP ) &&
              !net.diagWanted ( p2pf::P2PF_DIAGM_TRACE ),
              "...and IsDiagWanted answers about what was actually asked for" );

      // --- the round trip ----------------------------------------------
      Check ( net.log ( L"Sec27", L"P2PFTest: the first line" ) == S_OK,
              "RaiseDiag writes into the same stream" );
      Check ( nMine == 1,
              "...and the sink is called, once" );
      {
        std::lock_guard<std::mutex> lk ( oCap );
        Check ( strMod == L"Sec27" && strText == L"P2PFTest: the first line",
                "...with the module and the sentence intact" );
      }
      Check ( nSeverity == p2pf::P2PF_DIAG_APP,
              "...and the severity the client chose" );
      Check ( nTime != 0,
              "...carrying the kernel's own creation time, stamped when the "
              "event was made rather than when it was delivered" );
      Check ( nEventNo != 0,
              "...and a serial number -- the FACADE's, because the kernel's own "
              "GetEvent is documented as auto-assigned and is assigned nowhere" );

      // THE measurement of this pass. Delivery is inline, on the raising
      // thread -- so a line raised here is delivered here, with no pump, no
      // queue and no hop.
      Check ( nThread == nMainThread,
              "delivery is SYNCHRONOUS on the thread that raised it -- the one "
              "callback in this ABI that is not marshalled to a pump" );

      unsigned int nFirstNo = nEventNo;
      net.log ( L"Sec27", L"P2PFTest: the second line" );
      Check ( nMine == 2 && nEventNo > nFirstNo,
              "...which ADVANCES per event, so a gap is the count of what this "
              "sink's own mask threw away" );

      // --- the current-record rule -------------------------------------
      Check ( hrBadPart == E_INVALIDARG,
              "GetDiagText refuses a part it does not have" );
      {
        std::lock_guard<std::mutex> lk ( oCap );
        Check ( !strClass.empty(),
                "...but answers the kernel's own name for the class" );
      }
      unsigned int cchOut = 0, uHr = 0;
      Check ( net.raw()->GetDiagText ( p2pf::P2PF_DIAGT_MESSAGE, nullptr, &cchOut )
                == p2pf::P2PF_E_NO_DIAG,
              "...and OUTSIDE a delivery it is P2PF_E_NO_DIAG, not an empty "
              "string -- 'nothing was attached' and 'wrong place' are different "
              "answers" );
      Check ( net.raw()->GetDiagInfo ( &uHr, nullptr, nullptr )
                == p2pf::P2PF_E_NO_DIAG,
              "...GetDiagInfo says the same" );

      // --- the mask is a filter, and it is ours -------------------------
      int nBefore = nMine;
      net.raiseDiag ( p2pf::P2PF_DIAG_ERROR, L"Sec27", L"P2PFTest: filtered out" );
      Check ( nMine == nBefore,
              "a class outside the mask is not delivered" );
      Check ( net.diagMask ( p2pf::P2PF_DIAGM_ALL ) == S_OK,
              "SetDiagMask widens an existing subscription" );
      net.raiseDiag ( p2pf::P2PF_DIAG_ERROR, L"Sec27", L"P2PFTest: now audible" );
      Check ( nMine == nBefore + 1,
              "...and the same class arrives once it is in the mask" );
      {
        std::lock_guard<std::mutex> lk ( oCap );
        Check ( strClass == L"EVERR",
                "...reading as the kernel's own EVERR, so the severity mapping "
                "is the kernel's and not this facade's invention" );
      }

      // --- the feedback breaker ----------------------------------------
      // A logging handler that logs. The kernel cannot be stopped from raising
      // during a delivery, but the facade's own front door can be, and this is
      // the check that it is: the answer is S_FALSE and the count does not run
      // away.
      int nAtLoop = nMine;
      net.onDiag ( [&]( const p2pf::DiagEvent& d )
      {
          if ( ::wcsncmp ( d.text(), L"P2PFTest:", 9 ) != 0 ) return;
          ++nMine;
          hrInsideRaise = net.raiseDiag ( p2pf::P2PF_DIAG_APP, L"Sec27"
                                        , L"P2PFTest: from inside the handler" );
      }, p2pf::P2PF_DIAGM_ALL );
      net.log ( L"Sec27", L"P2PFTest: the line that logs" );
      Check ( hrInsideRaise == S_FALSE,
              "RaiseDiag from INSIDE a delivery is suppressed, not raised" );
      Check ( nMine == nAtLoop + 1,
              "...so the handler is entered exactly once and does not feed "
              "itself" );

      // --- unregistering from inside the handler ------------------------
      // The quiesce wait in SetDiagSink counts deliveries in flight, and this
      // thread IS one of them: a wait that did not know that would sit out its
      // whole budget every time. Bounded either way, so what this measures is
      // that it is PROMPT.
      std::atomic<unsigned int> nUnregMs { 0 };
      net.onDiag ( [&]( const p2pf::DiagEvent& d )
      {
          if ( ::wcsncmp ( d.text(), L"P2PFTest:", 9 ) != 0 ) return;
          unsigned int t0 = ::GetTickCount();
          net.onDiag ( nullptr );
          nUnregMs = ::GetTickCount() - t0;
      }, p2pf::P2PF_DIAGM_ALL );
      net.log ( L"Sec27", L"P2PFTest: unregister from within" );
      Check ( nUnregMs < 100,
              "unregistering from inside the handler returns at once rather "
              "than waiting for a delivery that is this very thread" );

      // --- and then nothing -------------------------------------------
      int nAfterUnreg = nMine;
      net.log ( L"Sec27", L"P2PFTest: nobody is listening" );
      Check ( nMine == nAfterUnreg,
              "an unregistered sink hears nothing" );
      Check ( net.diagMask() == 0 && !net.diagWanted ( p2pf::P2PF_DIAGM_ALL ),
              "...and says so through the mask and IsDiagWanted" );
      Check ( net.raw()->SetDiagMask ( p2pf::P2PF_DIAGM_ALL ) == S_FALSE,
              "SetDiagMask with no sink is S_FALSE -- recorded, not refused" );
      Check ( net.raw()->SetDiagSink ( nullptr, 0x80000000u ) == E_INVALIDARG,
              "a mask claiming the kernel's reserved top bit is refused" );
      Check ( net.raw()->RaiseDiag ( p2pf::P2PF_DIAG_APP, L"Sec27", L"" )
                == E_INVALIDARG,
              "an empty sentence is refused: a log line with no line is a bug "
              "in the caller" );

      // --- what the KERNEL says, and from which thread -------------------
      // The whole point of the feature: events this test did not raise. A
      // message to an address nothing answers to is the cheapest reliable
      // provocation -- section 22 already established that the kernel bounces
      // it, and a bounce is narrated.
      std::atomic<int>          nKernel  { 0 };
      std::atomic<unsigned int> nPumpTid { 0 };
      std::atomic<int>          nFromPump { 0 };
      net.onDiag ( [&]( const p2pf::DiagEvent& d )
      {
          if ( ::wcsncmp ( d.text(), L"P2PFPump:", 9 ) == 0 )
          {
            ++nFromPump;
            nPumpTid = d.threadId();
            return;
          }
          if ( ::wcsncmp ( d.text(), L"P2PFTest:", 9 ) != 0 && d.text()[0] )
            ++nKernel;
      }, p2pf::P2PF_DIAGM_ALL );

      {
        Flag oPosted;
        p2pf::Hub dSrv = net.createHub ( L"Diag" );
        p2pf::Hub dCli = net.createHub ( L"Diag.Node" );
        Check ( net.link ( L"Diag", L"Diag.Node" ) == S_OK,
                "an in-process edge to provoke the kernel with" );

        Flag oUp;
        dCli.onPeerUp ( [&](const wchar_t*){ oUp.Set(); } );
        oUp.Wait ( 10000 );

        // Raised ON THE PUMP: the same synchronous rule, seen from the other
        // side. A sink that was marshalled anywhere could not report this
        // thread id.
        dCli.onPost ( [&](unsigned int, void*)
        {
            net.raiseDiag ( p2pf::P2PF_DIAG_APP, L"Sec27"
                          , L"P2PFPump: raised on the pump thread" );
            oPosted.Set();
        } );
        dCli.post ( 1, nullptr );
        oPosted.Wait ( 5000 );

        Check ( nFromPump == 1 && nPumpTid != 0 && nPumpTid != nMainThread,
                "an event raised on a PUMP thread is delivered on that thread, "
                "not on the client's -- which is the same rule read backwards" );

        // Something the kernel itself has to complain about.
        dCli.sendText ( L"Diag.Nobody", L"nowhere", L"x" );
        for ( int i = 0; i < 60 && nKernel == 0; ++i )
          ::Sleep ( 25 );
        Check ( nKernel > 0,
                "the KERNEL's own narration arrives -- events this test did "
                "not raise, which is the whole of what ABI 10 added" );

        dCli.close();
        dSrv.close();
      }

      net.onDiag ( nullptr );
      Check ( net.diagMask() == 0,
              "the sink is taken back before the objects it captured go away" );
    }

    std::printf ( "\n=== %s (%d failure%s) ===\n",
                  g_nFailed ? "FAIL" : "PASS",
                  g_nFailed, g_nFailed == 1 ? "" : "s" );
    return g_nFailed ? 1 : 0;
}
