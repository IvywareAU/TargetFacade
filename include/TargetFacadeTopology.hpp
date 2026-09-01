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
// TargetFacadeTopology.hpp
//
// OPTIONAL header-only helper: a whole process's hubs, endpoints and edges
// from one block of text.
//
// This is Opus C5 ("topology bootstrap"), and it is deliberately a LAYER ABOVE
// the facade rather than DLL surface -- which is what the plan recommended and
// why it waited for the deployment map to land first.  Everything here is
// composed from calls a client could already make: CreateHub, SetEndpointMap,
// Link.  Nothing new crosses the ABI, and nothing here is required: the flat
// header and Fn.hpp are unchanged and complete without it.
//
//     p2pf::Network  net;
//     p2pf::Topology topo;
//
//     unsigned int badLine = 0;
//     if ( FAILED ( p2pf::Topology::parse ( kTopologyText, topo, &badLine ) ) )
//         ...                                  // reports the line, 1-based
//
//     topo.createHubs ( net );                 // hubs exist, nothing armed
//     topo.hub ( L"Demo.Server" )->onTopic ( L"chat", ... );   // <- the point
//     topo.arm ( net );                        // map pushed, edges armed
//
// TWO PHASES, not one call, and that is the whole design.  Handlers must be
// registered BEFORE anything is armed (Fn.hpp says so: the registries are
// written on the client thread and read on the pump thread with
// registration-then-read discipline).  A single applyTopology() would create
// hubs and arm edges with no window in between, so the convenience would cost
// the one ordering rule this layer cannot bend.
//
// ---------------------------------------------------------------------------
// The format -- line oriented, like the endpoint map it feeds:
//
//     # comments with '#' or ';'; blank lines ignored; whitespace irrelevant
//
//     hub  Demo.Server = tcp://127.0.0.1:7788   ; a hub THIS process owns,
//     hub  Demo.Client                          ;   with or without an endpoint
//     at   Demo.Remote = pipe://P2PmsgRemote    ; where a NON-local address is
//     link Demo.Server -> Demo.Client           ; listener -> dialer
//     link Demo.Server -> Demo.Edge = dmx://X   ; ...with an endpoint of its own
//
//   hub   declares a hub to create.  An optional "= endpoint" is exactly an
//         `at` line for the same address, written once.
//   at    a deployment-map entry for an address this process does NOT own --
//         a peer on another machine, reached by an edge armed elsewhere.
//   link  one edge.  THE ARROW IS NOT DECORATION: left is the listener, right
//         is the dialer, and IP2PNetwork::Link is not symmetric.
//
// An edge's endpoint is chosen most-specific-first: the link line's own
// endpoint, else the map entry for the LISTENER, else empty -- which is an
// in-process Dmx link derived from the address pair.  That is the same
// precedence the facade itself uses for an omitted endpoint.
//
// ---------------------------------------------------------------------------
// WHAT THIS FILE DOES NOT DO: parse endpoints.  The endpoint grammar lives
// inside the DLL on purpose -- a client-side copy would put that capability on
// the wrong side of the ABI and give every consumer its own dialect.  So the
// `= ...` halves are collected verbatim into map text and handed to
// SetEndpointMap, which is the one parser; only STRUCTURE (verbs, arrows,
// duplicate hubs, links naming hubs that were never declared) is checked here.
// When the DLL rejects an endpoint it reports a line number in the text IT was
// given, so parse() keeps a line-for-line back-map and translates it into a
// line number in YOUR file.
#pragma once

#include "TargetFacadeFn.hpp"

namespace p2pf {

// ---------------------------------------------------------------------------
// Topology -- parsed, then applied in two phases.
// ---------------------------------------------------------------------------
class Topology
{
  public:
    struct Edge
    {
        std::wstring listener;
        std::wstring dialer;
        std::wstring endpoint;      // empty: use the map, else in-process
    };

    // Parse only.  No hub is created and no endpoint is validated, so a
    // malformed file fails before anything in the process has changed.
    // *badLine (optional) is 1-based and counts blanks and comments, so it is
    // the number an editor shows.
    static HRESULT parse ( const wchar_t *text, Topology& out,
                           unsigned int *badLine = nullptr )
    {
        if ( badLine ) *badLine = 0;

        Topology t;
        if ( !text ) { out = std::move ( t ); return S_OK; }

        std::wstring      all ( text );
        std::wstring      line;
        unsigned int      lineNo = 0;
        std::wstring::size_type pos = 0;

        while ( pos <= all.size() )
        {
            std::wstring::size_type nl = all.find_first_of ( L"\r\n", pos );
            if ( nl == std::wstring::npos ) nl = all.size();
            line = trim ( all.substr ( pos, nl - pos ) );

            // Step past the terminator (\r\n counts once) before any continue.
            if ( nl < all.size() && all[nl] == L'\r' &&
                 nl + 1 < all.size() && all[nl+1] == L'\n' ) pos = nl + 2;
            else if ( nl < all.size() )                      pos = nl + 1;
            else                                             pos = all.size() + 1;

            ++lineNo;
            if ( line.empty() || line[0] == L'#' || line[0] == L';' )
                continue;

            std::wstring verb = trim ( token ( line, 0 ) );
            std::wstring rest = trim ( line.substr ( verb.size() ) );
            lower ( verb );

            if ( verb == L"hub" || verb == L"at" )
            {
                std::wstring addr = rest, ep;
                split ( rest, L'=', addr, ep );
                addr = trim ( addr );
                ep   = trim ( ep );

                if ( addr.empty() || looksLikeEndpoint ( addr ) )
                    return fail ( badLine, lineNo );
                if ( verb == L"at" && ep.empty() )
                    return fail ( badLine, lineNo );   // `at` with nothing to say
                if ( verb == L"hub" )
                {
                    if ( t.isHub ( addr ) )
                        return fail ( badLine, lineNo );   // declared twice
                    t.m_hubs.push_back ( addr );
                }
                if ( !ep.empty() )
                {
                    if ( t.hasEntry ( addr ) )
                        return fail ( badLine, lineNo );   // two endpoints, one address
                    t.m_mapText += addr + L" = " + ep + L"\n";
                    t.m_mapLines.push_back ( lineNo );
                    t.m_entries.push_back ( addr );
                }
                continue;
            }

            if ( verb == L"link" )
            {
                std::wstring pair = rest, ep;
                split ( rest, L'=', pair, ep );

                std::wstring::size_type arrow = pair.find ( L"->" );
                if ( arrow == std::wstring::npos )
                    return fail ( badLine, lineNo );

                Edge e;
                e.listener = trim ( pair.substr ( 0, arrow ) );
                e.dialer   = trim ( pair.substr ( arrow + 2 ) );
                e.endpoint = trim ( ep );

                if ( e.listener.empty() || e.dialer.empty() ||
                     e.listener == e.dialer ||
                     looksLikeEndpoint ( e.listener ) ||
                     looksLikeEndpoint ( e.dialer ) )
                    return fail ( badLine, lineNo );

                // Link requires BOTH ends to be live hubs of this network, so
                // an edge naming something never declared cannot work -- and
                // catching it here beats P2PF_E_NO_HUB after half the topology
                // is already armed.
                if ( !t.isHub ( e.listener ) || !t.isHub ( e.dialer ) )
                    return fail ( badLine, lineNo );

                t.m_edges.push_back ( e );
                continue;
            }

            return fail ( badLine, lineNo );           // unknown verb
        }

        out = std::move ( t );
        return S_OK;
    }

    // Phase 1.  Every hub, none of them armed -- so handlers can still be
    // registered.  On failure the hubs already created are closed, so a failed
    // createHubs leaves the process as it found it.
    HRESULT createHubs ( Network& net )
    {
        for ( size_t i = 0; i < m_hubs.size(); ++i )
        {
            try
            {
                m_live.emplace ( m_hubs[i], net.createHub ( m_hubs[i].c_str() ) );
            }
            catch ( ... )
            {
                closeAll();
                return P2PF_E_HUB_DUPLICATE;   // the one failure createHub has
            }
        }
        return S_OK;
    }

    // The hub at `address`, for registering handlers between the two phases.
    // NULL if the topology never declared it.
    Hub* hub ( const wchar_t *address )
    {
        auto it = m_live.find ( address ? address : L"" );
        return ( it == m_live.end() ) ? nullptr : &it->second;
    }

    // Phase 2.  Push the endpoint map, then arm every edge in file order,
    // listener side first (Link does that part).
    //
    // *badLine, if the map is rejected, is a line number in YOUR text.
    HRESULT arm ( Network& net, unsigned int *badLine = nullptr )
    {
        if ( badLine ) *badLine = 0;

        if ( !m_mapText.empty() )
        {
            unsigned int mapLine = 0;
            HRESULT hr = net.setEndpointMap ( m_mapText, &mapLine );
            if ( FAILED(hr) )
            {
                // Translate: the DLL counted lines in the text it was given.
                if ( badLine && mapLine >= 1 && mapLine <= m_mapLines.size() )
                    *badLine = m_mapLines[mapLine - 1];
                return hr;
            }
        }

        HRESULT hrWorst = S_OK;
        for ( size_t i = 0; i < m_edges.size(); ++i )
        {
            const Edge& e = m_edges[i];

            // Most specific first: the line's own endpoint, then what the map
            // says about the LISTENER, then nothing -- which Link reads as an
            // in-process Dmx edge derived from the pair.
            std::wstring ep = e.endpoint;
            if ( ep.empty() )
                ep = net.endpointFor ( e.listener.c_str() );

            HRESULT hr = net.link ( e.listener.c_str(), e.dialer.c_str(),
                                    ep.empty() ? nullptr : ep.c_str() );
            if ( FAILED(hr) )
                return hr;                      // stop at the first real failure
            if ( hr != S_OK )
                hrWorst = hr;                   // P2PF_S_UNRELATED_LINK, kept
        }
        return hrWorst;
    }

    // Close every hub this topology created, in reverse order of creation.
    void closeAll ( )
    {
        for ( size_t i = m_hubs.size(); i-- > 0; )
        {
            auto it = m_live.find ( m_hubs[i] );
            if ( it != m_live.end() ) it->second.close();
        }
        m_live.clear();
    }

    const std::vector<std::wstring>& hubs  ( ) const { return m_hubs; }
    const std::vector<Edge>&         edges ( ) const { return m_edges; }

    // The endpoint-map text this topology would push, for a log or a check.
    const std::wstring& mapText ( ) const { return m_mapText; }

  private:
    static HRESULT fail ( unsigned int *badLine, unsigned int lineNo )
    {
        if ( badLine ) *badLine = lineNo;
        return P2PF_E_ENDPOINT;
    }

    static std::wstring trim ( const std::wstring& s )
    {
        std::wstring::size_type b = s.find_first_not_of ( L" \t" );
        if ( b == std::wstring::npos ) return std::wstring();
        std::wstring::size_type e = s.find_last_not_of ( L" \t" );
        return s.substr ( b, e - b + 1 );
    }
    static std::wstring token ( const std::wstring& s, std::wstring::size_type from )
    {
        std::wstring::size_type b = s.find_first_not_of ( L" \t", from );
        if ( b == std::wstring::npos ) return std::wstring();
        std::wstring::size_type e = s.find_first_of ( L" \t", b );
        return s.substr ( b, ( e == std::wstring::npos ) ? std::wstring::npos : e - b );
    }
    static void split ( const std::wstring& s, wchar_t sep,
                        std::wstring& left, std::wstring& right )
    {
        std::wstring::size_type at = s.find ( sep );
        if ( at == std::wstring::npos ) { left = s; right.clear(); return; }
        left  = s.substr ( 0, at );
        right = s.substr ( at + 1 );
    }
    static void lower ( std::wstring& s )
    {
        for ( size_t i = 0; i < s.size(); ++i )
            if ( s[i] >= L'A' && s[i] <= L'Z' ) s[i] = (wchar_t)( s[i] - L'A' + L'a' );
    }
    // The same guard the arming verbs use on an address slot: a ':' or a "//"
    // means an endpoint was written where an address belongs.
    static bool looksLikeEndpoint ( const std::wstring& s )
    {
        return s.find ( L':' ) != std::wstring::npos ||
               s.find ( L"//" ) != std::wstring::npos;
    }

    bool isHub ( const std::wstring& a ) const
    {
        for ( size_t i = 0; i < m_hubs.size(); ++i ) if ( m_hubs[i] == a ) return true;
        return false;
    }
    bool hasEntry ( const std::wstring& a ) const
    {
        for ( size_t i = 0; i < m_entries.size(); ++i ) if ( m_entries[i] == a ) return true;
        return false;
    }

    std::vector<std::wstring>    m_hubs;      // in declaration order
    std::vector<std::wstring>    m_entries;   // addresses with an endpoint
    std::vector<Edge>            m_edges;
    std::wstring                 m_mapText;   // what arm() feeds SetEndpointMap
    std::vector<unsigned int>    m_mapLines;  // map line N came from file line
    std::map<std::wstring, Hub>  m_live;      // created by createHubs
};

} // namespace p2pf
