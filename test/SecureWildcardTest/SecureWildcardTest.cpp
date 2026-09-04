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
// SecureWildcardTest.cpp
//
// DOES A SECURE HUB'S WILDCARD LISTENER ACTUALLY REFUSE A STRANGER?
//
// This is the one question about P2PF_HUB_SECURE that a single process cannot
// answer, and the reason is structural rather than awkward: both ends of a
// Link are hubs the facade provisions ITSELF, so an in-process test can only
// ever produce peers that are already trusted.  The interesting peer is the
// one that turns up uninvited, and it has to come from somewhere the facade
// did not make.  So this test is two processes, over a real TCP socket.
//
// WHAT IS BEING SEPARATED.  A secure hub listening on "SW.*" performs two
// checks that are easy to conflate, and only one of them is new:
//
//     the PATTERN     decides who may CLAIM a name.  It is the accept filter,
//                     applied in OnLogin (P2PeerCon.cpp:4007-4016) to the name
//                     the dialer declared.  WildcardListenTest covers this
//                     thoroughly and WITHOUT authentication.
//     the ALLOW-LIST  decides whose signature is ACCEPTED.  Keyed on the
//                     source address off the wire (P2PeerCon.cpp:3615-3627 ->
//                     AuthPolicy::VerifyLogin -> PeerKeyCount(pSrc)), and
//                     compared exactly -- it never globs.
//
// So the rogue here is named SW.Rogue: it SATISFIES the pattern.  If it is
// refused, the pattern is not what refused it.  And the control that gives
// that its meaning is the third dial below -- the same rogue name, the same
// transport, the same pattern shape, against a PLAIN hub, where it comes
// straight up.  One difference between the two runs, and it is the flag.
//
//   S1  a secure hub's NAMED listener admits a peer from another process
//       whose published key it holds                    -- the positive case
//   S2  ...and its WILDCARD listener refuses a stranger that satisfies the
//       pattern but is not in the allow-list                 -- the question
//   S3  ...while a PLAIN hub's wildcard listener admits that identical
//       stranger                       -- the control: S2 was authentication
//
// HOW THE KEYS GET ACROSS, which is the part that has to be real.  A secure
// hub publishes "<stem>.key.pub" and "<stem>.agree.pub" when it is created,
// and a secure Listen/Connect reads the far end's two files out of the same
// directory.  Across two machines an operator copies them; here both
// processes are pointed at one directory with SetSecurityDir, which is the
// same exchange with the copying already done.  Note the ORDER that forces:
// the parent cannot listen for SW.Trusted until SW.Trusted has published, so
// the child is run once in --provision mode first.  That is not a quirk of
// the test; it is what provisioning IS.
//
//   SecureWildcardTest.exe                          run the whole thing
//   SecureWildcardTest.exe --provision <addr> <dir> (spawned) publish and exit
//   SecureWildcardTest.exe --dial <addr> <port> <dir> <secure|plain>
//
// Exit code 0 = pass.

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "TargetFacade.h"
#include "TargetFacadeFn.hpp"

// ---------------------------------------------------------------------------
// Ports, chosen above everything FacadeSmokeTest uses (7801-7833) so the two
// suites can run at the same time.
// ---------------------------------------------------------------------------
static const int kPortNamed    = 7841;   // the secure hub's NAMED listener
static const int kPortSecureWc = 7842;   // the secure hub's WILDCARD listener
static const int kPortPlainWc  = 7843;   // the plain hub's WILDCARD listener

// How long a dial is given to come up. Generous, because the whole meaning of
// S2 rests on "it did not come up" -- a wait too short to be sure would make
// the headline assertion a coin toss. S1 and S3 come up in well under a second
// and are the evidence that this budget is not simply always expiring.
static const unsigned int kUpMillisecs = 8000;

static int g_fails = 0;

static void Check ( bool ok, const char *what )
{
    std::printf ( "  [%s] %s\n", ok ? "PASS" : "FAIL", what );
    if ( !ok ) ++g_fails;
}

// ---------------------------------------------------------------------------
// Every peer name one hub saw log in.
//
// Locked, because OnPeerUp arrives on that hub's PUMP THREAD and every read
// here is from the main one -- which is the ordinary contract for every
// callback in this ABI, and the reason a bare std::set would be a data race
// rather than a shortcut.
// ---------------------------------------------------------------------------
class PeerLog
{
  public:
     PeerLog ( ) { ::InitializeCriticalSection ( &m_cs ); }
    ~PeerLog ( ) { ::DeleteCriticalSection     ( &m_cs ); }

    PeerLog             ( const PeerLog& ) = delete;
    PeerLog& operator = ( const PeerLog& ) = delete;

    void Add ( const wchar_t *peer )
    {
        ::EnterCriticalSection ( &m_cs );
        m_set.insert ( peer ? peer : L"" );
        ::LeaveCriticalSection ( &m_cs );
    }
    bool Has ( const wchar_t *peer )
    {
        ::EnterCriticalSection ( &m_cs );
        const bool b = m_set.find ( peer ) != m_set.end();
        ::LeaveCriticalSection ( &m_cs );
        return b;
    }

  private:
    std::set<std::wstring> m_set;
    CRITICAL_SECTION       m_cs;
};

// ---------------------------------------------------------------------------
// The security directory: one per RUN, beside the executable, wiped on the way
// in.
//
// A fixed directory would let a previous run's keys decide this one's result,
// and for a test whose headline is "the stranger was refused" that is the
// failure mode worth engineering out: SW.Rogue left in an allow-list by an
// earlier experiment would make S2 pass for the wrong reason. Deleted and
// remade, so every run provisions from nothing.
// ---------------------------------------------------------------------------
static std::wstring ExeDir ( )
{
    wchar_t wszPath [ MAX_PATH + 1 ] = { 0 };
    const DWORD n = ::GetModuleFileNameW ( 0, wszPath, MAX_PATH );
    if ( n == 0 || n > MAX_PATH )
      return std::wstring();
    std::wstring s ( wszPath );
    const size_t i = s.find_last_of ( L'\\' );
    return ( i == std::wstring::npos ) ? std::wstring() : s.substr ( 0, i + 1 );
}

static bool FileExists ( const std::wstring& path )
{
    return ::GetFileAttributesW ( path.c_str() ) != INVALID_FILE_ATTRIBUTES;
}

// Does this allow-list carry a line for `who`? Read as bytes and matched at
// the start of a line, which is all the structure needed to answer "is this
// peer trusted" from outside the library.
static bool AllowListNames ( const std::wstring& path, const char *who )
{
    FILE *pf = 0;
    if ( ::_wfopen_s ( &pf, path.c_str(), L"rb" ) != 0 || !pf )
      return false;

    std::string text;
    char buf [ 1024 ];
    size_t n;
    while ( ( n = ::fread ( buf, 1, sizeof(buf), pf ) ) > 0 )
      text.append ( buf, n );
    ::fclose ( pf );

    const std::string needle ( who );
    for ( size_t i = 0; ( i = text.find ( needle, i ) ) != std::string::npos; i += needle.size() )
    {
      const bool bLineStart = ( i == 0 || text[i-1] == '\n' || text[i-1] == '\r' );
      const size_t j = i + needle.size();
      const bool bWholeField = ( j >= text.size() || text[j] == ' ' || text[j] == '\t' );
      if ( bLineStart && bWholeField )
        return true;
    }
    return false;
}

static void RemoveTree ( const std::wstring& dir )
{
    WIN32_FIND_DATAW fd;
    const std::wstring glob = dir + L"*";
    HANDLE h = ::FindFirstFileW ( glob.c_str(), &fd );
    if ( h != INVALID_HANDLE_VALUE )
    {
      do
      {
        if ( ::wcscmp ( fd.cFileName, L"." ) == 0 || ::wcscmp ( fd.cFileName, L".." ) == 0 )
          continue;
        ::DeleteFileW ( ( dir + fd.cFileName ).c_str() );
      } while ( ::FindNextFileW ( h, &fd ) );
      ::FindClose ( h );
    }
    ::RemoveDirectoryW ( dir.c_str() );
}

// ---------------------------------------------------------------------------
// The security directory as it may safely be QUOTED on a command line.
//
// A directory string here ends with a separator, and a quoted argument whose
// last character is a backslash does not mean what it looks like: that
// backslash escapes the closing quote, so the argument swallows the rest of
// the line and every argument after it shifts down one. That is not a cosmetic
// bug -- it cost a fork bomb to find, because a --dial child then arrived one
// argument short, failed to match its own mode, and ran the PARENT path
// instead, spawning three more of itself, each of which did the same.
//
// Both halves of that are fixed, and the second one is the one that matters:
// the trailing separator is dropped here (SetSecurityDir puts one back), AND
// wmain below now refuses to run the parent for ANY argv it was given, so a
// mode that fails to match can never again be read as "no arguments".
// ---------------------------------------------------------------------------
static std::wstring Quotable ( const std::wstring& dir )
{
    std::wstring s = dir;
    while ( !s.empty() && ( s[s.size()-1] == L'\\' || s[s.size()-1] == L'/' ) )
      s.erase ( s.size() - 1 );
    return s;
}

// ---------------------------------------------------------------------------
// Run this same executable again, and wait for it.
//
// SPAWNING OURSELVES rather than shipping a second .exe: the child needs the
// identical facade build, and a separate binary is one more thing to keep in
// step for no gain. The mode is an argv switch, so what runs in the child is
// visible in this file.
// ---------------------------------------------------------------------------
static DWORD RunChild ( const std::wstring& args, unsigned int timeoutMs )
{
    wchar_t wszExe [ MAX_PATH + 1 ] = { 0 };
    if ( ::GetModuleFileNameW ( 0, wszExe, MAX_PATH ) == 0 )
      return (DWORD)-1;

    std::wstring cmd = L"\"";
    cmd += wszExe;
    cmd += L"\" ";
    cmd += args;

    STARTUPINFOW si; ::memset ( &si, 0, sizeof(si) ); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ::memset ( &pi, 0, sizeof(pi) );

    if ( !::CreateProcessW ( 0, &cmd[0], 0, 0, FALSE, 0, 0, 0, &si, &pi ) )
      return (DWORD)-1;

    DWORD dwExit = (DWORD)-1;
    if ( ::WaitForSingleObject ( pi.hProcess, timeoutMs ) == WAIT_OBJECT_0 )
      ::GetExitCodeProcess ( pi.hProcess, &dwExit );
    else
      ::TerminateProcess ( pi.hProcess, (UINT)-2 );

    ::CloseHandle ( pi.hThread );
    ::CloseHandle ( pi.hProcess );
    return dwExit;
}

// ---------------------------------------------------------------------------
// CHILD: publish this address's key material and exit.
//
// Creating a secure hub IS the provisioning -- the identity, the agreement key
// and both publishable halves are written before the pump exists -- so there
// is nothing to do here but make one and let it go. Run before the parent
// tries to listen for this address, because a secure Listen reads the file
// this writes.
// ---------------------------------------------------------------------------
static int ChildProvision ( const wchar_t *addr, const wchar_t *dir )
{
    try
    {
      p2pf::Network net;
      if ( FAILED ( net.setSecurityDir ( dir ) ) )
        return 2;
      p2pf::Hub h = net.createHub ( addr, p2pf::P2PF_HUB_SECURE );
      const bool bKeys = !h.securityFingerprint ( ).empty ( );
      h.close ( );
      return bKeys ? 0 : 3;
    }
    catch ( ... ) { return 4; }
}

// ---------------------------------------------------------------------------
// CHILD: dial the parent and report whether the login completed.
//
// Exit 0 means the peer came up. ANY other code means it did not, and the
// caller must not care which -- "refused" and "never got that far" are the
// same observation from here, and the facade says so: a refused login is
// dropped with no client-visible event, so there is nothing to distinguish
// them by and nothing that should try.
//
// `secure` picks which hub this child is. It is the only difference between
// the run that must be refused and the control that must not.
// ---------------------------------------------------------------------------
static int ChildDial ( const wchar_t *addr, int port, const wchar_t *dir, bool bSecure )
{
    try
    {
      p2pf::Network net;
      if ( FAILED ( net.setSecurityDir ( dir ) ) )
        return 2;

      p2pf::Hub h = net.createHub ( addr, bSecure ? p2pf::P2PF_HUB_SECURE
                                                  : p2pf::P2PF_HUB_SPAWN_PUMP );

      wchar_t wszEp [ 64 ];
      ::swprintf_s ( wszEp, L"tcp://127.0.0.1:%d", port );

      // The parent's address is the first label of ours: SW.Trusted dials SW.
      std::wstring peer ( addr );
      const size_t iDot = peer.find ( L'.' );
      if ( iDot != std::wstring::npos )
        peer = peer.substr ( 0, iDot );

      if ( FAILED ( h.connect ( peer.c_str(), wszEp ) ) )
        { h.close ( ); return 5; }

      const ULONGLONG t0 = ::GetTickCount64 ( );
      while ( ::GetTickCount64 ( ) - t0 < kUpMillisecs )
      {
        if ( h.isPeerUp ( peer.c_str() ) )
          { h.close ( ); return 0; }
        ::Sleep ( 50 );
      }
      h.close ( );
      return 6;                       // never came up
    }
    catch ( ... ) { return 4; }
}

// ---------------------------------------------------------------------------
// PARENT
// ---------------------------------------------------------------------------
static int Parent ( )
{
    const std::wstring dir = ExeDir ( ) + L"p2p-securewc\\";
    RemoveTree ( dir );

    std::printf ( "\n== SecureWildcardTest: a stranger at a secure wildcard listener ==\n" );
    std::wprintf ( L"   keys: %s\n\n", dir.c_str() );

    // The trusted peer publishes first, because the parent cannot listen for a
    // peer whose key it does not hold. This is the provisioning step in its
    // honest form -- across machines it is an operator copying two files.
    const std::wstring provArgs = L"--provision SW.Trusted \"" + Quotable ( dir ) + L"\"";
    Check ( RunChild ( provArgs, 30000 ) == 0,
            "the trusted peer ran once and published its key files" );

    try
    {
      p2pf::Network net;
      if ( FAILED ( net.setSecurityDir ( dir.c_str() ) ) )
        { std::printf ( "  [FAIL] setSecurityDir\n" ); return 1; }

      // --- the secure hub ---------------------------------------------------
      p2pf::Hub sec = net.createHub ( L"SW", p2pf::P2PF_HUB_SECURE );

      // WHAT THE LISTENING SIDE SAW, recorded as it happens rather than asked
      // for afterwards.
      //
      // IsPeerUp cannot answer this and it is worth saying why, because the
      // first draft of this test used it and got a false FAIL out of a
      // connection that had worked perfectly. RunChild WAITS for the child,
      // and the child closes its hub before it exits -- so by the time the
      // parent could ask, the peer it is asking about is gone whatever
      // happened. OnPeerUp fires at the moment of the login and the record
      // outlives the connection, which is the only shape that can distinguish
      // "never logged in" from "logged in and then went away".
      //
      // It also proves the ADOPTED NAME reached this side: a wildcard listener
      // reports the name the dialer claimed, not the pattern it matched.
      PeerLog secSeen, plainSeen;
      sec.onPeerUp ( [&](const wchar_t *p){ secSeen.Add ( p ); } );

      wchar_t wszNamed[64], wszWc[64], wszPlainWc[64];
      ::swprintf_s ( wszNamed,   L"tcp://:%d", kPortNamed    );
      ::swprintf_s ( wszWc,      L"tcp://:%d", kPortSecureWc );
      ::swprintf_s ( wszPlainWc, L"tcp://:%d", kPortPlainWc  );

      // A NAMED listener first, which is what puts SW.Trusted in the allow-list
      // and turns enforcement on. The wildcard could not be first: with an
      // empty allow-list the hub has never armed, and would take anyone.
      Check ( SUCCEEDED ( sec.listen ( L"SW.Trusted", wszNamed ) ),
              "the secure hub armed a NAMED listener for the peer it trusts" );
      Check ( ( sec.securityFlags ( ) & p2pf::P2PF_SEC_ARMED ) != 0,
              "...so it is now enforcing" );
      Check ( SUCCEEDED ( sec.listen ( L"SW.*", wszWc ) ),
              "...and only then may it arm the WILDCARD listener" );

      // --- the plain hub, for the control ----------------------------------
      p2pf::Hub plain = net.createHub ( L"PW" );
      plain.onPeerUp ( [&](const wchar_t *p){ plainSeen.Add ( p ); } );
      Check ( SUCCEEDED ( plain.listen ( L"PW.*", wszPlainWc ) ),
              "a PLAIN hub arms the same wildcard shape with no provisioning" );

      // --- S1: the trusted peer, from its own process -----------------------
      wchar_t wszArgs [ 512 ];
      ::swprintf_s ( wszArgs, L"--dial SW.Trusted %d \"%s\" secure", kPortNamed, Quotable ( dir ).c_str() );
      Check ( RunChild ( wszArgs, 60000 ) == 0,
              "S1  a secure hub admits a peer from ANOTHER PROCESS whose key it holds" );
      Check ( secSeen.Has ( L"SW.Trusted" ),
              "...and the LISTENING side saw it log in, over a real TCP socket" );

      // --- S2: the stranger, which satisfies the pattern --------------------
      //
      // SW.Rogue matches SW.*, so the accept filter lets it through. What it
      // does not have is a line in SW's allow-list -- nothing ever put one
      // there, because the facade only ever trusts a peer it was armed for.
      ::swprintf_s ( wszArgs, L"--dial SW.Rogue %d \"%s\" secure", kPortSecureWc, Quotable ( dir ).c_str() );
      const DWORD dwRogue = RunChild ( wszArgs, 60000 );
      std::printf ( "        rogue child exit = %lu (6 = dialed, never logged in)\n", dwRogue );

      // THE EXACT CODE, not merely "non-zero", and this is the difference
      // between a test and a test that always passes. A rogue that failed to
      // build a hub (4), could not read the security directory (2) or whose
      // Connect was refused outright (5) would also be non-zero, and would
      // prove nothing at all about authentication -- the interesting run is
      // the one where the socket connected, the login was sent, and the peer
      // still never came up. That is 6, and only 6.
      Check ( dwRogue == 6,
              "S2  ...and REFUSES a stranger that satisfies the pattern" );
      Check ( !secSeen.Has ( L"SW.Rogue" ),
              "...and the listening side never saw it log in at all" );

      // AND IT WAS NOT REFUSED FOR HAVING NO KEY, which is the last competing
      // explanation and the easiest one to leave standing by accident. The
      // rogue is a fully provisioned secure hub: it published its own identity,
      // and its own allow-list trusts SW, so it signed its login perfectly
      // well. The single thing it does not have is a line in SW's allow-list.
      Check ( FileExists ( dir + L"SW.Rogue.key.pub" ),
              "...though the stranger HAD a valid identity of its own" );
      Check ( AllowListNames ( dir + L"SW.allow", "SW.Trusted" ) &&
             !AllowListNames ( dir + L"SW.allow", "SW.Rogue"   ),
              "...and the only thing it lacked was a line in the allow-list" );

      // --- S3: the control --------------------------------------------------
      //
      // The same name shape, the same transport, the same pattern, against a
      // hub that differs by one flag. If this fails, S2 proved nothing: the
      // refusal would have been the pattern, the port or the dial rather than
      // the authentication.
      ::swprintf_s ( wszArgs, L"--dial PW.Rogue %d \"%s\" plain", kPortPlainWc, Quotable ( dir ).c_str() );
      Check ( RunChild ( wszArgs, 60000 ) == 0,
              "S3  a PLAIN hub's wildcard admits that identical stranger" );
      Check ( plainSeen.Has ( L"PW.Rogue" ),
              "...so what refused it in S2 was AUTHENTICATION, not the pattern" );

      // The secure hub is still enforcing and still holds its trusted peer:
      // being knocked on by a stranger changed nothing about it.
      Check ( ( sec.securityFlags ( ) & p2pf::P2PF_SEC_ARMED ) != 0,
              "the secure hub is still armed after refusing the stranger" );

      plain.close ( );
      sec.close ( );
    }
    catch ( const std::exception& e )
    {
      std::printf ( "  [FAIL] exception: %s\n", e.what() );
      ++g_fails;
    }

    std::printf ( "\n%d failure(s)\n", g_fails );
    return g_fails;
}

//
//  ANY ARGUMENT AT ALL MEANS THIS IS A CHILD, and that is the whole of the
//  dispatch rule.  The obvious spelling -- try each mode, fall through to the
//  parent -- makes "the arguments did not parse" and "there were no arguments"
//  the same case, and this program's parent path SPAWNS THREE COPIES OF
//  ITSELF.  One mis-quoted path turned that into a fork bomb.  So a child that
//  cannot understand its own arguments exits loudly and spawns nothing; only a
//  bare invocation is ever the parent.
//
int wmain ( int argc, wchar_t **argv )
{
    if ( argc == 1 )
      return Parent ( );

    if ( argc == 4 && ::wcscmp ( argv[1], L"--provision" ) == 0 )
      return ChildProvision ( argv[2], argv[3] );

    if ( argc == 6 && ::wcscmp ( argv[1], L"--dial" ) == 0 )
      return ChildDial ( argv[2], ::_wtoi ( argv[3] ), argv[4],
                         ::wcscmp ( argv[5], L"secure" ) == 0 );

    std::fwprintf ( stderr, L"SecureWildcardTest: unrecognised arguments (argc=%d). "
                            L"This is a child invocation and will NOT run the parent.\n"
                  , argc );
    for ( int i = 0; i < argc; ++i )
      std::fwprintf ( stderr, L"  argv[%d] = [%s]\n", i, argv[i] );
    return 90;
}
