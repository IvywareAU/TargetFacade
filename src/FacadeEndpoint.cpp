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
// FacadeEndpoint.cpp -- endpoint grammar, peer guards, topology classifier.
#include "stdafx.h"
#include "FacadeEndpoint.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace {

// Kernel limits, checked here rather than left to the silent (short)
// narrowing an integer parameter would do.
const unsigned int kPortMax    = 65535;
const unsigned int kComPortMax =   255;   // P2PeerCon232::SetComPort formats
                                          // \\.\COM%d, so COM10+ is fine

//
//  Scan a run of decimal digits
//  NOTES: Hand-rolled rather than _wtoi/wcstoul: those are locale-sensitive
//         at the edges and answer 0 for "not a number", which is also a
//         legal-looking port.  This says "how many digits did I consume",
//         so the caller can insist the whole segment was numeric
//       : Saturates instead of wrapping; every caller range-checks anyway
//
bool
ScanUInt ( const wchar_t *lpsz, unsigned int& rValue )
{
    if ( !lpsz || !*lpsz )
      return false;

    unsigned __int64 uAcc = 0;
    for ( const wchar_t *p = lpsz; *p; ++p )
    {
      if ( *p < L'0' || *p > L'9' )
        return false;
      uAcc = uAcc * 10 + (unsigned)( *p - L'0' );
      if ( uAcc > 0xFFFFFFFFui64 )
        uAcc = 0xFFFFFFFFui64;
    }
    rValue = (unsigned int)uAcc;
    return true;
}

// Case-insensitive compare of a scheme token; the scheme is ASCII by
// construction, so no locale is involved.
bool
IsScheme ( const CString& csToken, const wchar_t *lpszWanted )
{
    return csToken.CompareNoCase ( lpszWanted ) == 0;
}

// A listen never names a host.  The facade has no way to say "bind this NIC",
// so "tcp://10.0.0.7:7788" as a LISTEN would read as one and do nothing of the
// sort.  What a listen MAY say is which any-address it means, because that is
// the family: "0.0.0.0" is IPv4's and "::" is IPv6's.
bool
IsAnyHost4 ( const CString& csHost )
{
    return csHost.IsEmpty()          ||
           csHost == L"*"            ||
           csHost == L"0.0.0.0";
}

// TRUE for a well-formed RFC4291 literal, with an optional %zone on a dial
// (a link-local peer needs one to be reachable at all).  getaddrinfo() is what
// will finally read it; this only refuses what it would refuse later and less
// helpfully, a Connect armed against a host that can never resolve.
bool
IsIPv6Literal ( const CString& csHost, bool bAllowZone )
{
    CString csAddr = csHost;
    int nZone = csAddr.Find ( L'%' );
    if ( nZone >= 0 )
    {
      if ( !bAllowZone || nZone == csAddr.GetLength() - 1 )
        return false;
      csAddr = csAddr.Left ( nZone );
    }
    IN6_ADDR oAddr;
    return ::InetPtonW ( AF_INET6, (LPCWSTR)csAddr, &oAddr ) == 1;
}

// TRUE for "::" in any spelling ("::", "0::0", "0:0:0:0:0:0:0:0").
bool
IsIPv6Any ( const CString& csHost )
{
    IN6_ADDR oAddr;
    if ( ::InetPtonW ( AF_INET6, (LPCWSTR)csHost, &oAddr ) != 1 )
      return false;
    for ( int i = 0; i < 16; ++i )
      if ( oAddr.s6_addr[i] )
        return false;
    return true;
}

//
//  TRUE when lpszChild names a hub below lpszParent in the address tree
//  NOTES: Prefix on a DOT BOUNDARY, matching P2Paddr::IsChild -- so skip
//         levels are children too ("A.B.C" is a child of "A"), which is
//         exactly the kernel's own routing rule and why the topology check
//         here is permissive rather than restrictive
//       : Case-sensitive, like every other P2Paddr comparison (the
//         case-folding branch is commented out at P2Peer.cpp:478)
//
bool
IsBelow ( const wchar_t *lpszParent, const wchar_t *lpszChild )
{
    size_t uParent = ::wcslen ( lpszParent );
    if ( !uParent )
      return false;
    return ::wcsncmp ( lpszParent, lpszChild, uParent ) == 0 &&
           lpszChild[uParent] == L'.';
}

} // namespace

// Characters that make a peer a P2Padomain PATTERN rather than a literal
// address (P2Peer.cpp:450-491, :531, :611-725).  Their presence means the far
// side's real name is chosen at login, so nothing about the address tree can
// be concluded at arm time.
//
// OUT of the anonymous namespace above, and deliberately: a SECURE hub has to
// ask this before it arms anything (FacadeHub::AdmitPatternPeer), because a
// pattern is the one peer whose key cannot be looked up.  It is declared in
// FacadeEndpoint.h for that one caller.
bool
IsPattern ( const wchar_t *lpsz )
{
    for ( const wchar_t *p = lpsz; *p; ++p )
      if ( *p == L'*' || *p == L'?' || *p == L'#' ||
           *p == L'|' || *p == L'<' || *p == L'>' || *p == L',' )
        return true;
    return false;
}

///////////////////////////////////////////////////////////////////////
//  Formatting

CString
FacadeEndpoint::Format ( ) const
{
    CString csOut;
    switch ( eKind )
    {
      case p2pfTcp:
      {
        // The scheme carries the family, so a v6 endpoint that dialled by
        // NAME still says v6 when it is read back.  A literal is bracketed
        // whenever it holds a ':', which is the only thing the port separator
        // could be confused with.
        const wchar_t *lpszScheme = eFamily == p2pfFamIPv6 ? L"tcp6"
                                  : eFamily == p2pfFamDual ? L"tcp46"
                                  :                          L"tcp";
        if ( csHost.Find ( L':' ) >= 0 )
          csOut.Format ( L"%s://[%s]:%u", lpszScheme, (LPCWSTR)csHost, uNum );
        else
          csOut.Format ( L"%s://%s:%u",   lpszScheme, (LPCWSTR)csHost, uNum );
        break;
      }
      case p2pfPipe:   csOut.Format ( L"pipe://%s",   (LPCWSTR)csName );       break;
      case p2pfDmx:    csOut.Format ( L"dmx://%s",    (LPCWSTR)csName );       break;
      case p2pfSerial: csOut.Format ( L"serial://COM%u", uNum );               break;
    }
    return csOut;
}

///////////////////////////////////////////////////////////////////////
//  Parsing

//
//  Parse "scheme://target" (or "scheme:target") into a FacadeEndpoint
//  NOTES: The target of pipe:// and dmx:// is taken VERBATIM -- a pipe
//         constant is routinely a full \\.\pipe\name path and a Dmx service
//         name is an arbitrary application string.  Only tcp and serial have
//         internal structure worth parsing
//       : Only the outer whitespace is trimmed.  A name is otherwise passed
//         through byte for byte, because the kernel compares service names
//         and pipe names by equality and any normalisation here would be a
//         second, divergent policy
//
HRESULT
ParseFacadeEndpoint ( const wchar_t *lpszEndpoint
                    , bool bListen, FacadeEndpoint& rOut )
{
    if ( !lpszEndpoint || !*lpszEndpoint )
      return p2pf::P2PF_E_ENDPOINT;

    CString csAll ( lpszEndpoint );
    csAll.Trim();

    int nColon = csAll.Find ( L':' );
    if ( nColon <= 0 )
      return p2pf::P2PF_E_ENDPOINT;         // no scheme at all

    CString csScheme = csAll.Left ( nColon );
    CString csTarget = csAll.Mid  ( nColon + 1 );
    if ( csTarget.Left(2) == L"//" )
      csTarget = csTarget.Mid ( 2 );

    // --- tcp -------------------------------------------------------------
    //  tcp:// is IPv4 unless the host is a bracketed IPv6 literal, which is
    //  how an RFC3986 URI spells one: tcp://[::1]:7788 dials v6, and a LISTEN
    //  on tcp://[::]:7788 is one dual-stack socket serving both families.
    //  tcp6:// and tcp46:// say the family outright, which is what a dial BY
    //  NAME needs - the family is also what the resolver may answer, AAAA for
    //  tcp6 and either for tcp46.  tcp4:// refuses a v6 literal rather than
    //  quietly changing family under a scheme that names one
    const bool bTcpAuto = IsScheme ( csScheme, L"tcp"   );
    const bool bTcp4    = IsScheme ( csScheme, L"tcp4"  ) ||
                          IsScheme ( csScheme, L"ipv4"  );
    const bool bTcp6    = IsScheme ( csScheme, L"tcp6"  ) ||
                          IsScheme ( csScheme, L"ipv6"  );
    const bool bTcp46   = IsScheme ( csScheme, L"tcp46" );
    if ( bTcpAuto || bTcp4 || bTcp6 || bTcp46 )
    {
      CString csHost, csPort;
      bool    bBracketed = false;
      if ( csTarget.Left(1) == L"[" )
      {
        int nClose = csTarget.Find ( L']' );
        if ( nClose < 0 || csTarget.Mid ( nClose + 1, 1 ) != L":" )
          return p2pf::P2PF_E_ENDPOINT;     // "[::1]" with no port, or junk
        csHost     = csTarget.Mid ( 1, nClose - 1 );
        csPort     = csTarget.Mid ( nClose + 2 );
        bBracketed = true;
        if ( csHost.IsEmpty() )
          return p2pf::P2PF_E_ENDPOINT;
      }
      else
      {
        int nPortSep = csTarget.ReverseFind ( L':' );
        if ( nPortSep < 0 )
          return p2pf::P2PF_E_ENDPOINT;     // "tcp://host" -- no port
        csHost = csTarget.Left ( nPortSep );
        csPort = csTarget.Mid  ( nPortSep + 1 );
        // An unbracketed ':' in the host is a v6 literal whose last group
        // has been read as the port.  Refused, never guessed at
        if ( csHost.Find ( L':' ) >= 0 || csHost.Find ( L']' ) >= 0 )
          return p2pf::P2PF_E_ENDPOINT;
      }

      unsigned int uPort = 0;
      if ( !ScanUInt ( csPort, uPort ) )
        return p2pf::P2PF_E_ENDPOINT;
      if ( uPort < 1 || uPort > kPortMax )
        return p2pf::P2PF_E_ENDPOINT;

      // Brackets hold an IPv6 literal and nothing else -- not a name, and
      // not a v4 address
      if ( bBracketed && !IsIPv6Literal ( csHost, !bListen ) )
        return p2pf::P2PF_E_ENDPOINT;
      if ( bTcp4 && bBracketed )
        return p2pf::P2PF_E_ENDPOINT;

      FacadeFamily eFamily = bTcp6  ? p2pfFamIPv6
                           : bTcp46 ? p2pfFamDual
                           :          p2pfFamIPv4;

      if ( bListen )
      {
        if ( bBracketed )
        {
          if ( !IsIPv6Any ( csHost ) )
            return p2pf::P2PF_E_ENDPOINT;   // a listen cannot pick a NIC
          if ( bTcpAuto )
            eFamily = p2pfFamDual;          // "[::]" is every address there is
        }
        else if ( csHost == L"0.0.0.0" )
        {
          if ( !bTcpAuto && !bTcp4 )
            return p2pf::P2PF_E_ENDPOINT;   // v4's any-address on a v6 scheme
        }
        else if ( !IsAnyHost4 ( csHost ) )
          return p2pf::P2PF_E_ENDPOINT;     // a listen cannot pick a NIC
        csHost.Empty();
      }
      else
      {
        if ( csHost.IsEmpty() )
          return p2pf::P2PF_E_ENDPOINT;     // a dial needs somewhere to go
        if ( bBracketed && bTcpAuto )
          eFamily = p2pfFamIPv6;            // the literal IS the family
        // A dotted quad under tcp6 can never be dialled: an AF_INET6 socket
        // with IPV6_V6ONLY set has no v4 path.  Refused here rather than
        // left to fail every redial
        IN_ADDR oV4;
        if ( eFamily == p2pfFamIPv6 &&
             ::InetPtonW ( AF_INET, (LPCWSTR)csHost, &oV4 ) == 1 )
          return p2pf::P2PF_E_ENDPOINT;
      }

      rOut.eKind   = p2pfTcp;
      rOut.eFamily = eFamily;
      rOut.csHost  = csHost;
      rOut.csName.Empty();
      rOut.uNum    = uPort;
      return S_OK;
    }

    // --- pipe / dmx ------------------------------------------------------
    if ( IsScheme ( csScheme, L"pipe" ) || IsScheme ( csScheme, L"dmx" ) )
    {
      if ( csTarget.IsEmpty() )
        return p2pf::P2PF_E_ENDPOINT;

      rOut.eKind  = IsScheme ( csScheme, L"pipe" ) ? p2pfPipe : p2pfDmx;
      rOut.csName = csTarget;
      rOut.csHost.Empty();
      rOut.uNum   = 0;
      rOut.eFamily = p2pfFamIPv4;
      return S_OK;
    }

    // --- serial ----------------------------------------------------------
    if ( IsScheme ( csScheme, L"serial" ) || IsScheme ( csScheme, L"com" ) )
    {
      CString csNum = csTarget;
      if ( csNum.Left(3).CompareNoCase ( L"COM" ) == 0 )
        csNum = csNum.Mid ( 3 );

      unsigned int uCom = 0;
      if ( !ScanUInt ( csNum, uCom ) )
        return p2pf::P2PF_E_ENDPOINT;
      if ( uCom < 1 || uCom > kComPortMax )
        return p2pf::P2PF_E_ENDPOINT;

      rOut.eKind  = p2pfSerial;
      rOut.uNum   = uCom;
      rOut.eFamily = p2pfFamIPv4;
      rOut.csHost.Empty();
      rOut.csName.Empty();
      return S_OK;
    }

    return p2pf::P2PF_E_ENDPOINT;           // unknown scheme
}

///////////////////////////////////////////////////////////////////////
//  The configured endpoint map

//
//  Parse a multi-line "address = endpoint" block
//  NOTES: ALL-OR-NOTHING.  A configuration file is applied as a unit or not at
//         all: half a deployment map is worse than none, because the entries
//         that did land would make the ones that did not look like a routing
//         problem rather than a typo.  So this fills a LOCAL map and only
//         swaps it into the caller's on the way out
//       : The line number is 1-based and counts blanks and comments, so it is
//         the number an editor shows.  That is the whole value of validating
//         here: "line 7" beats P2PF_E_ENDPOINT from a Connect three files away
//       : Values are stored canonically (Format()), not as the caller typed
//         them, so "TCP://10.0.0.7:7788" and "tcp://10.0.0.7:7788" are one
//         entry and the read side matches what the arming verbs would report
//
HRESULT
ParseFacadeEndpointMap ( const wchar_t *lpszText
                       , FacadeEndpointMap& rOut
                       , unsigned int *puBadLine )
{
    if ( puBadLine )
      *puBadLine = 0;

    FacadeEndpointMap mapLocal;
    if ( !lpszText || !*lpszText )         // an empty map is a legal map
    {
      rOut.swap ( mapLocal );
      return S_OK;
    }

    unsigned int  uLine = 0;
    const wchar_t *p    = lpszText;
    for ( ;; )                             // every path breaks at the NUL
    {
      // One line, however it is terminated: \n, \r\n or the end of the text.
      const wchar_t *pEnd = p;
      while ( *pEnd && *pEnd != L'\n' && *pEnd != L'\r' )
        ++pEnd;

      ++uLine;
      CString csLine ( p, (int)( pEnd - p ) );
      csLine.Trim();

      // Advance past the terminator before anything can `continue`.
      if ( *pEnd == L'\r' && *(pEnd+1) == L'\n' ) p = pEnd + 2;
      else if ( *pEnd )                           p = pEnd + 1;
      else                                        p = pEnd;

      if ( csLine.IsEmpty() || csLine[0] == L'#' || csLine[0] == L';' )
      {
        if ( !*pEnd ) break;
        continue;
      }

      // FIRST '=' splits: an endpoint may not contain one, but a Dmx service
      // name or a pipe name is an arbitrary application string and might.
      int nEq = csLine.Find ( L'=' );
      if ( nEq <= 0 )
      {
        if ( puBadLine ) *puBadLine = uLine;
        return p2pf::P2PF_E_ENDPOINT;
      }

      CString csAddr = csLine.Left ( nEq );              csAddr.Trim();
      CString csEp   = csLine.Mid  ( nEq + 1 );          csEp.Trim();

      FacadeEndpoint oEp;
      if ( csAddr.IsEmpty() ||
           IsSwappedPeerArgument ( (LPCWSTR)csAddr ) ||
           FAILED ( ParseFacadeEndpoint ( (LPCWSTR)csEp, false, oEp ) ) )
      {
        if ( puBadLine ) *puBadLine = uLine;
        return p2pf::P2PF_E_ENDPOINT;
      }

      // Serial cannot be expressed as ONE endpoint, for the same reason Link
      // refuses it: a null-modem link is two different local ports, COM5 at
      // one end and COM6 at the other, each opened exclusively. An entry that
      // named one port would arm the listener correctly and then hand the
      // dialer the port the listener is already holding.
      if ( oEp.eKind == p2pfSerial )
      {
        if ( puBadLine ) *puBadLine = uLine;
        return p2pf::P2PF_E_ENDPOINT;
      }

      // A repeated key in a config file is a mistake, and silently keeping one
      // of the two would be a deployment that behaves differently from the way
      // it reads.
      if ( mapLocal.find ( (LPCWSTR)csAddr ) != mapLocal.end() )
      {
        if ( puBadLine ) *puBadLine = uLine;
        return p2pf::P2PF_E_ENDPOINT;
      }

      mapLocal[(LPCWSTR)csAddr] = (LPCWSTR)oEp.Format();

      if ( !*pEnd )
        break;
    }

    rOut.swap ( mapLocal );
    return S_OK;
}

///////////////////////////////////////////////////////////////////////
//  Guards

bool
IsSwappedPeerArgument ( const wchar_t *lpszPeer )
{
    if ( !lpszPeer )
      return false;
    return ::wcschr ( lpszPeer, L':' ) != 0 ||
           ::wcsstr ( lpszPeer, L"//" ) != 0;
}

FacadeRelation
ClassifyPeer ( const wchar_t *lpszAddress, const wchar_t *lpszPeer )
{
    if ( !lpszAddress || !*lpszAddress || !lpszPeer || !*lpszPeer )
      return p2pfRelPattern;                // nothing to conclude
    if ( IsPattern ( lpszPeer ) )
      return p2pfRelPattern;
    if ( ::wcscmp ( lpszAddress, lpszPeer ) == 0 )
      return p2pfRelSelf;
    if ( IsBelow ( lpszAddress, lpszPeer ) )
      return p2pfRelDescendant;
    if ( IsBelow ( lpszPeer, lpszAddress ) )
      return p2pfRelAncestor;
    return p2pfRelUnrelated;
}

CString
DeriveDmxService ( const wchar_t *lpszListener, const wchar_t *lpszDialer )
{
    CString csOut;
    csOut.Format ( L"P2PF|%s|%s", lpszListener, lpszDialer );
    return csOut;
}
