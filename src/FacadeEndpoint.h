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
// FacadeEndpoint.h -- the endpoint grammar behind IP2PHub::Listen/Connect.
//
// This is the whole of what the eight typed Listen*/Connect* verbs this pair
// replaced ever disagreed about: which kernel connection class to build and
// what to stuff into it.
// Everything downstream (PostCon, the message map, routing, events) has
// already forgotten which transport it was.
//
// The parse lives INSIDE the DLL on purpose.  The point of a string endpoint
// is that it can be a configuration value -- an ini entry, a registry value,
// an argv element -- that changes transport with no rebuild anywhere.  A
// client-side parser would put that capability on the wrong side of the ABI:
// every consumer (C++, the ATL layer, scripts) would need its own copy.
#pragma once

#include "TargetFacade.h"      // the P2PF_E_* codes this grammar returns

#include <map>
#include <string>

// Which kernel connection class an endpoint names.
enum FacadeTransport
{
    p2pfTcp,        // P2PeerConWsa    -- AF_INET only
    p2pfPipe,       // P2PeerConPipe
    p2pfDmx,        // P2PeerConDmx    -- same process
    p2pfSerial      // P2PeerCon232
};

// A parsed endpoint.  Flat and copyable: it is built on a caller thread and
// consumed there too (the factories do no OS work), so it never crosses a
// thread or the DLL boundary.
struct FacadeEndpoint
{
    FacadeTransport eKind;
    CString         csHost;     // tcp dial only; empty on a listen
    CString         csName;     // pipe name / dmx service, verbatim
    unsigned int    uNum;       // tcp port, or COM number

    // TRUE when the facade RESOLVED this endpoint from an omitted one, FALSE
    // when a caller spelled it out.  Not part of the endpoint's identity --
    // Format() ignores it, and the same endpoint may arrive either way -- but
    // it is the whole difference between two kinds of failed dial: a typed
    // endpoint that is not there is a configuration fault and must fail fast,
    // while a derived one has already been proved to name a live sibling hub
    // (FacadeNetwork::ResolveDial), so the only thing left to fail is timing.
    // FacadeHub::MakeCon is the only reader.
    bool            bDerived;

    FacadeEndpoint ( ) : eKind ( p2pfTcp ), uNum ( 0 ), bDerived ( false ) { }

    // The canonical spelling of this endpoint -- what ParseFacadeEndpoint
    // would accept back, so an endpoint recorded at arm time round-trips.
    CString
      Format ( ) const;
};

// Parse one endpoint string.  `bListen` only affects the tcp host rule (a
// listen must not name a host, a dial must).  P2PF_E_ENDPOINT on anything
// malformed, out of range, or unsupported by this kernel.
HRESULT
  ParseFacadeEndpoint ( const wchar_t *lpszEndpoint
                      , bool bListen, FacadeEndpoint& rOut );

// The configured endpoint map: ADDRESS -> the canonical DIAL endpoint that
// reaches it.  Keyed by address rather than by (hub, peer) because "where does
// Demo.Server live" is a fact about Demo.Server, not about who is asking -- and
// that is what makes one table serve both roles: a dial looks up its PEER, a
// listen looks up ITS OWN address and drops the host.
typedef std::map<std::wstring,std::wstring> FacadeEndpointMap;

// Parse a multi-line "address = endpoint" block into that map.
//
// One entry per line; blank lines and lines whose first non-space character is
// '#' or ';' are ignored, so an ini file or a here-doc can be passed through
// verbatim.  The value is stored CANONICALLY (what Format() would produce), so
// what goes in round-trips out.
//
// Everything is validated here rather than at the arming call site that later
// consumes it, because here is where the mistake was made.  ALL-OR-NOTHING: on
// any failure `rOut` is untouched and `*puBadLine` (optional) receives the
// 1-based line number, counting blanks and comments so it matches an editor.
//
// Rejected, each with P2PF_E_ENDPOINT:
//   * a line with no '='; an empty address; an address that is really an
//     endpoint (a ':' or "//" in it -- the same guard the arming verbs use);
//   * an endpoint that is not a legal DIAL form (so tcp must name a host);
//   * serial://, for the reason Link refuses it: a null-modem link is two
//     DIFFERENT local ports, one per side, and one entry cannot say that;
//   * the same address twice -- a duplicated key in a config file is a
//     mistake, not a last-one-wins.
HRESULT
  ParseFacadeEndpointMap ( const wchar_t *lpszText
                         , FacadeEndpointMap& rOut
                         , unsigned int *puBadLine );

// TRUE when `peer` cannot possibly be a P2Paddr: a colon or a "//" means the
// caller passed an endpoint where an address belongs.  A colon in an address
// is never legal -- the [VNetname:] qualifier in the P2Paddr grammar was
// designed and never implemented, so a leading "tcp:" is silently swallowed
// into the first hop's name and every message to that peer then dies as
// P2Pevent_UNDELIVERABLE with nothing reported at arm time.
bool
  IsSwappedPeerArgument ( const wchar_t *lpszPeer );

// TRUE when `peer` is a P2Padomain PATTERN rather than one address -- it holds
// one of the kernel's wildcard characters, so the far side's real name is
// chosen at LOGIN and nothing about it can be known at arm time.  What that
// costs a SECURE hub is the whole of FacadeHub::AdmitPatternPeer.
bool
  IsPattern ( const wchar_t *lpsz );

// How `peer` sits relative to this hub in the dotted address tree.
enum FacadeRelation
{
    p2pfRelSelf,        // the same address -- a hub cannot connect to itself
    p2pfRelDescendant,  // peer is below me   ("App" -> "App.B", "App.B.C")
    p2pfRelAncestor,    // peer is above me   ("App.B" -> "App")
    p2pfRelUnrelated,   // siblings, or different trees -- broadcasts die here
    p2pfRelPattern      // peer is a P2Padomain pattern; unknowable until login
};

FacadeRelation
  ClassifyPeer ( const wchar_t *lpszAddress, const wchar_t *lpszPeer );

// The Dmx service name an endpoint-less Listen/Connect resolves to, for one
// DIRECTED edge.  Both sides compute it from facts both already hold -- their
// own address and the peer's -- so nothing is published and nothing is looked
// up.  The ordered PAIR is in the key because the kernel's Dmx rendezvous
// matches on the service string alone (P2PeerConDmx.cpp:646-655: first
// SERVICE con with an equal name wins, peer address and owning hub ignored),
// so a name keyed on one address would cross-wire a hub expecting two peers.
CString
  DeriveDmxService ( const wchar_t *lpszListener, const wchar_t *lpszDialer );
