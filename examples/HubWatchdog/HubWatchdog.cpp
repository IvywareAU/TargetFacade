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
// HubWatchdog.cpp
//
// A worked example of everything facade ABI 6 and ABI 7 added, in the shape
// those additions were actually for: a SUPERVISOR hub that watches its peers.
//
//     Watch            the supervisor -- pings, evicts, keeps the tally
//     Watch.Node       an ordinary worker hub that answers
//     Watch.Zombie     a worker that stops answering, and gets dropped
//
// What each ABI 6 addition does here, and why it is the natural fit:
//
//   SetTimer / onTimer   the heartbeat. Housekeeping on the hub's own thread,
//                        with no extra thread to race it.
//   Ping                 the actual liveness question, answered end to end
//                        rather than inferred from "the socket is still open".
//   Post / onPost        the sweep's RESULT, handed back to the pump so the
//                        tally is only ever touched by one thread.
//   Disconnect           evicting one peer without tearing down the hub --
//                        which before ABI 6 was the only option there was.
//   SetConOption /       what the link actually is: dial or listen, encrypted
//   GetConOption         or not, and its receive limit.
//   onEvent              diagnostics with a CODE to branch on, instead of a
//                        sentence to pattern-match.
//   onMessageEx          declining a topic this hub does not own, so the
//                        kernel's own "unknown message" reporting still runs.
//   native               named, printed, and NOT cast -- see the note at the
//                        bottom of the file.
//
// What ABI 7 adds, and why a supervisor is exactly where it shows:
//
//   broadcastEx(tag)     the census goes out to everyone at once, STAMPED WITH
//                        THE SWEEP NUMBER.
//   reply / Message::tag a worker answers with replyText, which addresses the
//                        sender and carries that stamp back untouched. So an
//                        answer that arrives during sweep 4 announces which
//                        sweep it is answering -- and a slow worker's late
//                        reply is recognisable as late instead of being
//                        counted as fresh. Before this, correlating them meant
//                        putting a sequence number in the payload and agreeing
//                        a format for it at both ends.
//   P2PF_PRI_HIGH        the census jumps the queue on a busy hub.
//   P2PF_SEND_NO_BOUNCE  and it is fire-and-forget: a peer with no interest in
//                        `census` declines it, and with this flag that decline
//                        costs nothing and says nothing. Without it, every
//                        uninterested peer answers every sweep with a bounce.
//
// And what ABI 8 adds, which is the answer rather than the question:
//
//   createMessage /      the worker replies with a RECORD -- state, host, load
//   setText / set        -- as three named fields beside the payload, and NO
//                        FORMAT IS AGREED ANYWHERE. Before this, a reply
//                        carrying three values meant inventing an encoding for
//                        the payload and writing a parser for it at the other
//                        end, in both programs, kept in step by hand.
//   Message::fieldText / the supervisor reads the two fields it cares about by
//   Message::field       name, and would not notice if the worker added a
//                        fourth.
//
// And what ABI 10 adds, which is the log both halves were always missing:
//
//   onDiag               ONE stream. The supervisor's own "sweep 3 starting"
//                        and Targetcore's "not deliverable to [Watch.Zombie]"
//                        arrive at the same handler, in order, each stamped
//                        with the thread that raised it. Before this the
//                        kernel's half went nowhere at all.
//   log / raiseDiag      writing into it, which is what makes the interleave
//                        worth having -- a log you can only read is a log you
//                        have to correlate with your own by hand.
//   diagWanted           and the gate that keeps an expensive line unbuilt
//                        when nothing is listening.
//
// THE ONE RULE THIS EXAMPLE EXISTS TO SHOW: `onTimer` runs on the PUMP thread,
// and `ping` waits for that thread to deliver the answer -- so pinging from
// inside the timer handler would deadlock the hub for the whole budget. The
// facade refuses it outright (P2PF_E_PUMP_THREAD) rather than letting you find
// out in production. The timer therefore only WAKES the sweep; the sweep runs
// on this thread, and hands its findings back with post().
//
//     onTimer  (pump)  --SetEvent-->  sweep (main thread)  --post()-->  (pump)
//
// Build: part of TargetFacade(2026).sln. Includes only the facade's public
// headers -- no MFC, no Targetcore, no WinSock.
//
// Exit code 0 = the run did what this file says it does.

#include "TargetFacadeFn.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Keys for the two callbacks that carry one
// ---------------------------------------------------------------------------
const unsigned int kTimerHeartbeat = 1;   // SetTimer key
const unsigned int kPostPingResult = 10;  // Post key

const unsigned int kHeartbeatMs    = 500; // see the note on resolution below
const int          kMissesAllowed  = 2;   // strikes before a peer is evicted
const int          kSweeps         = 4;

// One ping result, handed from the sweep thread to the pump by pointer. It
// outlives the callback because the sweep owns it until onPost has run.
struct PingResult
{
    std::wstring peer;
    bool         answered;
    unsigned int millisecs;
};

// ---------------------------------------------------------------------------
// The supervisor's state. Touched ONLY on the pump thread -- that is the whole
// reason the sweep hands its results back with post() instead of writing them
// itself. No mutex appears anywhere near it.
// ---------------------------------------------------------------------------
struct Watchdog
{
    std::map<std::wstring,int> misses;      // peer -> consecutive failures
    std::vector<std::wstring>  evicted;     // pump decides, sweep carries out
    int                        beats = 0;   // heartbeats fired
};

int wmain ( )
{
    std::setvbuf ( stdout, nullptr, _IONBF, 0 );
    std::wprintf ( L"=== TargetFacade HubWatchdog (ABI %u) ===\n\n", p2pf::ABI_VERSION );

    int nProblems = 0;
    auto Say = [&] ( bool bOk, const wchar_t *what )
    {
        std::wprintf ( L"  [%s] %s\n", bOk ? L"ok " : L"BAD", what );
        if ( !bOk ) ++nProblems;
    };

    p2pf::Network net;

    // -----------------------------------------------------------------------
    // 0. The log, before anything else exists to write to it.        (ABI 10)
    //
    // A supervisor is exactly the program that wants this: it already reports
    // what it decided, and the kernel already reports what went wrong, and
    // until now those were two different places with no way to interleave
    // them. One sink, one stream, one thread-id column.
    //
    // THE RULE THIS DEMONSTRATES BY OBEYING IT: the handler runs on the thread
    // that RAISED the event -- a pump, this thread, a transport completion --
    // so its state is atomic and it does the least it can. That is also why
    // the last thing this program does is take the sink back: the counters it
    // captures die at the end of wmain, and the hubs are torn down after that.
    // -----------------------------------------------------------------------
    std::atomic<int> nDiagMine   { 0 };   // lines this program wrote
    std::atomic<int> nDiagKernel { 0 };   // lines Targetcore wrote

    net.onDiag ( [&] ( const p2pf::DiagEvent& d )
    {
        const bool bMine = ::wcsncmp ( d.text(), L"Watchdog:", 9 ) == 0;
        ( bMine ? nDiagMine : nDiagKernel ) += 1;
        std::wprintf ( L"      log[%s tid=%lu] %s%s%s\n"
                     , d.className().c_str()
                     , (unsigned long)d.threadId()
                     , d.module()
                     , *d.module() ? L": " : L""
                     , d.text() );
    }, p2pf::P2PF_DIAGM_PROBLEMS | p2pf::P2PF_DIAGM_APP );

    p2pf::Hub watch  = net.createHub ( L"Watch" );
    p2pf::Hub node   = net.createHub ( L"Watch.Node" );
    p2pf::Hub zombie = net.createHub ( L"Watch.Zombie" );

    Watchdog     dog;
    HANDLE       hSweep    = ::CreateEvent ( nullptr, FALSE, FALSE, nullptr );
    std::atomic<unsigned long> nPumpThread { 0 };

    // -----------------------------------------------------------------------
    // 1. The heartbeat.  Runs on the pump; does the LEAST it can.
    // -----------------------------------------------------------------------
    watch.onTimer ( [&] ( unsigned int, unsigned int key )
    {
        if ( key != kTimerHeartbeat ) return;

        nPumpThread = ::GetCurrentThreadId();
        ++dog.beats;                        // pump-owned, like everything in `dog`

        // Wake the sweep. NOT ping() -- that waits for this very thread to
        // deliver the answer, so the facade would (correctly) refuse it with
        // P2PF_E_PUMP_THREAD.
        ::SetEvent ( hSweep );

        // Re-arm: kernel timers are one-shot. Legal from inside the handler --
        // setTimer only POSTS to the pump, it never waits for it.
        if ( dog.beats < kSweeps )
            watch.setTimer ( kHeartbeatMs, kTimerHeartbeat );
    } );

    // -----------------------------------------------------------------------
    // 2. The sweep's results, applied on the pump thread
    // -----------------------------------------------------------------------
    watch.onPost ( [&] ( unsigned int key, void *ctx )
    {
        if ( key != kPostPingResult || ctx == nullptr ) return;

        // Sole writer of `dog`, and it is this callback -- one thread, no lock.
        PingResult *r = (PingResult*)ctx;
        if ( r->answered )
        {
            dog.misses[r->peer] = 0;
            std::wprintf ( L"      %-14s %4u ms\n", r->peer.c_str(), r->millisecs );
        }
        else if ( ++dog.misses[r->peer] > kMissesAllowed )
        {
            // Evicting one peer, with the hub and every other link untouched.
            // Before ABI 6 the only way to stop talking to a peer was Close(),
            // which would have taken the healthy links down with it.
            std::wprintf ( L"      %-14s no answer x%d -- evicting\n"
                         , r->peer.c_str(), dog.misses[r->peer] );
            dog.evicted.push_back ( r->peer );
            dog.misses.erase ( r->peer );
            // Disconnect waits on the pump, so it cannot run HERE either --
            // hand it back to the sweep thread by leaving the name in
            // `evicted`, which the sweep drains.
        }
        else
            std::wprintf ( L"      %-14s no answer (%d)\n"
                         , r->peer.c_str(), dog.misses[r->peer] );
    } );

    // -----------------------------------------------------------------------
    // 3. Coded diagnostics
    // -----------------------------------------------------------------------
    watch.onEvent ( [&] ( unsigned int code, const wchar_t *peer, const wchar_t *what )
    {
        // The code is the part to branch on. The sentence is for the log.
        const wchar_t *kind = ( code == p2pf::P2PF_EVT_UNRELATED_LINK ) ? L"topology"
                            : ( code == p2pf::P2PF_EVT_DIAL_GAVE_UP   ) ? L"dial"
                            : ( code == p2pf::P2PF_EVT_ROUTING_ERROR  ) ? L"routing"
                            :                                            L"hub";
        std::wprintf ( L"      [event/%s] peer='%s'\n", kind, peer );
        (void)what;   // the prose half; print it in a real log
    } );

    // -----------------------------------------------------------------------
    // 4. Answering, and declining
    // -----------------------------------------------------------------------
    std::atomic<int> nStatusSeen  { 0 };
    std::atomic<int> nStatusFresh { 0 };   // answered the sweep now running
    std::atomic<int> nStatusStale { 0 };   // answered an earlier one
    std::atomic<int> nRecordsRead { 0 };   // replies that carried a record
    std::atomic<unsigned int> nSweepTag { 0 };

    watch.onMessageEx ( [&] ( const p2pf::Message& m ) -> HRESULT
    {
        if ( std::wcscmp ( m.topic, L"status" ) == 0 )
        {
            ++nStatusSeen;
            // THE CORRELATION. `m.tag` is the sweep number this hub stamped
            // on the census, handed back by the worker's reply(). Nothing in
            // the payload says which sweep is being answered, and nothing has
            // to -- the two ends never agreed a format, only a tag.
            if ( m.tag == nSweepTag ) ++nStatusFresh;
            else                      ++nStatusStale;

            // The record, read by name (ABI 8). No format was agreed; the
            // worker set three fields and this reads the two it cares about.
            std::wstring strState = m.fieldText ( L"state" );
            unsigned int uLoad    = 0;
            std::vector<unsigned char> aLoad = m.field ( L"load" );
            if ( aLoad.size() == sizeof(uLoad) )
              std::memcpy ( &uLoad, &aLoad[0], sizeof(uLoad) );
            if ( !strState.empty() ) ++nRecordsRead;

            std::wprintf ( L"      %-14s answers sweep %u  state=%s load=%u%s\n"
                         , m.source, m.tag
                         , strState.empty() ? L"(none)" : strState.c_str(), uLoad
                         , ( m.tag == nSweepTag ) ? L"" : L"  (late)" );
            return S_OK;                    // handled: routing stops here
        }
        // Not ours. S_FALSE hands the message on to the kernel's own handler
        // chain, which is where a genuinely unknown topic gets reported as
        // unknown instead of being silently swallowed by this sink.
        return S_FALSE;
    } );

    node.onTopic ( L"census", [&] ( const p2pf::Message& m )
    {
        // THE ANSWER IS A RECORD, and nothing here agrees an encoding for it
        // (ABI 8). Three named fields beside the payload; the supervisor reads
        // them by name. Before this, "node ok" was all a reply could say
        // without both ends agreeing a private format for the payload.
        //
        // reply/replyMsg is send-to-the-sender-carrying-its-tag, which is the
        // whole of request/response here. The worker never learns what the tag
        // MEANS, and does not need to.
        p2pf::OutMessage rep = net.createMessage();
        rep.setText ( L"state", L"ok" );
        rep.setText ( L"host",  node.address() );
        unsigned int uLoad = 17 + ( m.tag % 5 );      // stand-in for real work
        rep.set     ( L"load", &uLoad, sizeof(uLoad) );
        node.replyMsg ( m, L"status", rep );
    } );

    // -----------------------------------------------------------------------
    // 5. Wire it up
    // -----------------------------------------------------------------------
    Say ( net.link ( L"Watch", L"Watch.Node"   ) == S_OK, L"linked Watch -> Watch.Node" );
    Say ( net.link ( L"Watch", L"Watch.Zombie" ) == S_OK, L"linked Watch -> Watch.Zombie" );

    for ( int i = 0; i < 100 && !( watch.isPeerUp ( L"Watch.Node" ) &&
                                   watch.isPeerUp ( L"Watch.Zombie" ) ); ++i )
        ::Sleep ( 50 );
    Say ( watch.isPeerUp ( L"Watch.Node" ) && watch.isPeerUp ( L"Watch.Zombie" )
        , L"both peers logged in" );

    // What the links ARE -- kernel facts, not the facade's bookkeeping.
    for ( const auto& c : watch.cons() )
    {
        unsigned int mode = watch.conOption ( c.peer.c_str(), p2pf::P2PF_OPT_MODE );
        unsigned int cap  = watch.conOption ( c.peer.c_str(), p2pf::P2PF_OPT_MAXRECV );
        std::wprintf ( L"      %-14s %s, recv<=%u bytes, %s\n"
                     , c.peer.c_str()
                     , mode == p2pf::P2PF_CONMODE_LISTEN ? L"listening"
                     : mode == p2pf::P2PF_CONMODE_DIAL   ? L"dialing" : L"accepted"
                     , cap
                     , watch.conEncrypted ( c.peer.c_str() ) ? L"encrypted" : L"clear" );
    }

    // A round trip before anything else, so the number below has a baseline.
    unsigned int ms = 0;
    Say ( watch.ping ( L"Watch.Node", 5000, &ms ) == S_OK, L"ping answered" );
    std::wprintf ( L"      first round trip: %u ms\n", ms );

    // The one call that must be refused, and the reason this example is
    // shaped the way it is. (Asked for here, on the main thread, it works --
    // this only shows what the guard is FOR.)
    Say ( watch.raw()->Ping ( L"Watch.Node", 100, &ms ) != p2pf::P2PF_E_PUMP_THREAD
        , L"ping from a client thread is fine" );

    std::wprintf ( L"\n-- heartbeat --\n" );

    // -----------------------------------------------------------------------
    // 6. Run
    // -----------------------------------------------------------------------
    // NOTE ON RESOLUTION: the kernel computes a timer's deadline from the top
    // of the CURRENT SECOND, so a 500 ms request can elapse almost at once or
    // take up to a second and a half. Housekeeping, never pacing.
    Say ( watch.setTimer ( kHeartbeatMs, kTimerHeartbeat ) != 0, L"heartbeat armed" );

    bool                      bZombieSilenced = false;
    int                       nSweep          = 0;
    std::vector<std::wstring> gone;            // sweep-side: already evicted

    for ( ;; )
    {
        if ( ::WaitForSingleObject ( hSweep, 30000 ) != WAIT_OBJECT_0 )
            break;                                    // the heartbeat stopped

        std::wprintf ( L"    sweep %d\n", ++nSweep );

        // After the first sweep, the zombie stops answering -- closing its hub
        // leaves the supervisor's connection armed and apparently healthy,
        // which is exactly the failure a ping detects and a socket does not.
        if ( nSweep == 1 && !bZombieSilenced )
        {
            zombie.close();
            bZombieSilenced = true;
            std::wprintf ( L"      (Watch.Zombie has gone away)\n" );
        }

        // Announce the sweep into the SAME stream the kernel narrates into,
        // so a reader of the log sees "sweep 2 starting" immediately above
        // whatever the kernel says went wrong during it -- which is the whole
        // argument for having one stream rather than two.
        {
            wchar_t szLine[128];
            std::swprintf ( szLine, 128, L"Watchdog: sweep %d starting", nSweep );
            net.log ( L"Sweep", szLine );
        }

        // THE SWEEP. On this thread, because ping() waits.
        std::vector<PingResult> results;
        for ( const auto& c : watch.cons() )
        {
            // A peer stays in cons() after Disconnect -- the hub still KNOWS
            // it, it simply has no connection for it any more, and the read
            // side reports what is true rather than what is tidy. A supervisor
            // is the one that decides a name is finished with.
            if ( std::find ( gone.begin(), gone.end(), c.peer ) != gone.end() )
                continue;

            PingResult r;
            r.peer      = c.peer;
            r.millisecs = 0;
            r.answered  = ( watch.ping ( c.peer.c_str(), 600, &r.millisecs ) == S_OK );
            results.push_back ( std::move ( r ) );
        }

        // Hand each result to the pump. The pointer stays valid because
        // `results` outlives the post -- the loop below waits for the pump to
        // drain before it goes out of scope.
        for ( auto& r : results )
            watch.post ( kPostPingResult, &r );

        // THE CENSUS. One broadcast to everyone still up, stamped with this
        // sweep's number, ahead of ordinary traffic, and asking for no bounce
        // from peers that do not answer censuses.
        //
        // It doubles as the drain marker it always was: it goes through the
        // same queue as the posts above, and FIFO order guarantees they have
        // been consumed by the time it is sent.
        nSweepTag = (unsigned int)nSweep;
        watch.broadcastEx ( L"census", "?", 1
                          , nSweepTag                        // the tag
                          , p2pf::P2PF_PRI_HIGH              // ahead of the rest
                          , p2pf::P2PF_SEND_NO_BOUNCE );     // fire and forget
        ::Sleep ( 250 );

        // Drain the evictions the pump asked for (Disconnect waits, so it
        // belongs on this thread, not in the callback that decided it).
        for ( const auto& peer : dog.evicted )
        {
            HRESULT hr = watch.disconnect ( peer.c_str() );
            std::wprintf ( L"      disconnect('%s') -> 0x%08lX\n", peer.c_str(), hr );
            gone.push_back ( peer );
        }
        if ( !dog.evicted.empty() ) dog.evicted.clear();

        if ( nSweep >= kSweeps ) break;
    }

    std::wprintf ( L"\n-- after --\n" );

    Say ( nStatusSeen > 0, L"onMessageEx handled the 'status' replies" );
    Say ( nStatusFresh > 0, L"...and each one named the sweep it was answering" );
    Say ( nStatusStale == 0, L"...with none of them stale" );
    Say ( nRecordsRead > 0, L"...and each carried a RECORD, read by field name" );
    Say ( !watch.isPeerUp ( L"Watch.Zombie" ), L"the zombie is gone" );
    Say ( watch.isPeerUp ( L"Watch.Node" ),    L"the healthy peer is untouched" );
    Say ( nPumpThread != 0 && nPumpThread != ::GetCurrentThreadId()
        , L"the heartbeat really ran on the pump thread" );

    std::wprintf ( L"      Describe():\n%s\n", watch.describe().c_str() );

    // Housekeeping the ABI could not ask for before.
    watch.closeIdleCons();

    // THE ESCAPE HATCH. This is a P2PeerHub*, and casting it is the point --
    // it is how a client reaches a kernel feature this facade does not expose
    // (a sub-target, a second pump, a custom P2Peerio). Doing so needs
    // Targetcore's headers, its import lib, MFC and its threading rules, and
    // nothing done through it is covered by any promise this layer makes.
    // Which is why this example only PRINTS it.
    void *pNative = watch.native();
    Say ( pNative != nullptr, L"native() hands back the P2PeerHub" );
    std::wprintf ( L"      P2PeerHub* = %p  (cast it and you are on your own)\n", pNative );

    // The log, checked and then taken back.                          (ABI 10)
    Say ( nDiagMine == kSweeps,
          L"every sweep announced itself into the kernel's own log stream" );
    Say ( net.diagWanted ( p2pf::P2PF_DIAGM_APP ),
          L"...and IsDiagWanted says so, which is what gates an expensive line" );
    std::wprintf ( L"      %d line(s) of it came from Targetcore itself\n",
                   (int)nDiagKernel );

    // UNREGISTER BEFORE THE CAPTURES DIE. The counters above go out of scope
    // at the end of this function and the hubs are torn down after that -- and
    // a teardown is one of the noisier things this kernel does.
    net.onDiag ( nullptr );

    ::CloseHandle ( hSweep );

    node.close();
    watch.close();

    std::wprintf ( L"\n=== %s ===\n", nProblems ? L"PROBLEMS" : L"OK" );
    return nProblems ? 1 : 0;
}
