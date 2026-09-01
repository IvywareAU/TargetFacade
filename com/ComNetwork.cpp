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
// ComNetwork.cpp -- implementation of coclass P2PNetwork.
#include "stdafx.h"
#include "ComNetwork.h"
#include "ComHub.h"
#include "ComMessage.h"

// Every IP2PNetworkCom entry point funnels its failures through this, so a
// script reads a sentence in Err.Description instead of a bare hex code.  The
// text and the mechanism live in ComHub.cpp, shared with the hub.
static inline HRESULT Fail ( HRESULT hr, LPCWSTR wszCall
                           , BSTR bsArg1 = NULL, BSTR bsArg2 = NULL, BSTR bsArg3 = NULL )
{
    return ComFail ( IID_IP2PNetworkCom, hr, wszCall, bsArg1, bsArg2, bsArg3 );
}

CP2PNetworkCom::CP2PNetworkCom ( )
    : m_pNet ( NULL )
{
}

HRESULT CP2PNetworkCom::FinalConstruct ( )
{
    // One call, and the kernel is up: StartupP2Pmsg + WSAStartup happen inside
    // the facade.  The ABI check turns a stale TargetFacade.dll next to us into
    // a clean error instead of a vtable skew.
    return ::P2PF_CreateNetwork ( p2pf::ABI_VERSION, &m_pNet );
}

void CP2PNetworkCom::FinalRelease ( )
{
    std::vector<IUnknown*> hubs;

    m_csHubs.Lock();
    hubs.swap ( m_hubs );
    m_csHubs.Unlock();

    for ( size_t i = 0; i < hubs.size(); ++i )
    {
        ATL::CComPtr<IP2PHubCom> spHub;
        if ( SUCCEEDED ( hubs[i]->QueryInterface ( IID_IP2PHubCom, (void**)&spHub ) ) )
            spHub->Close();                  // stops the dispatch thread and the pump
        hubs[i]->Release();
    }

    if ( m_pNet != NULL )
    {
        m_pNet->Release();                   // last one out shuts the kernel down
        m_pNet = NULL;
    }
}

STDMETHODIMP CP2PNetworkCom::CreateHub ( BSTR address, IP2PHubCom **ppHub )
{
    if ( ppHub == NULL ) return E_POINTER;
    *ppHub = NULL;
    if ( m_pNet == NULL ) return Fail ( p2pf::P2PF_E_CLOSED, L"CreateHub", address );
    if ( address == NULL || ::SysStringLen ( address ) == 0 )
        return Fail ( E_INVALIDARG, L"CreateHub", address );

    ATL::CComObject<CP2PHubCom> *pObj = NULL;
    HRESULT hr = ATL::CComObject<CP2PHubCom>::CreateInstance ( &pObj );
    if ( FAILED(hr) ) return Fail ( hr, L"CreateHub", address );

    ATL::CComPtr<IUnknown> spHold ( pObj->GetUnknown() );   // keep it alive through Init

    hr = pObj->Init ( m_pNet, this, address );
    if ( FAILED(hr) ) return Fail ( hr, L"CreateHub", address );

    // The network owns every hub it created, exactly as the facade does.
    IUnknown *pUnk = pObj->GetUnknown();
    pUnk->AddRef();
    m_csHubs.Lock();
    m_hubs.push_back ( pUnk );
    m_csHubs.Unlock();

    return pObj->QueryInterface ( IID_IP2PHubCom, (void**)ppHub );
}

//
// Make an empty message (facade ABI 8).
//
// Note what is missing compared with CreateHub above: no m_hubs entry, no
// AddRef held by the network, nothing for FinalRelease to close. A message
// holds no hub, no kernel object and no thread, so the network has nothing to
// own and the client releases it like any other COM object.
//
STDMETHODIMP CP2PNetworkCom::CreateMessage ( IP2PMessageCom **ppMessage )
{
    if ( ppMessage == NULL ) return E_POINTER;
    *ppMessage = NULL;
    if ( m_pNet == NULL ) return Fail ( p2pf::P2PF_E_CLOSED, L"CreateMessage" );

    ATL::CComObject<CP2PMessageCom> *pObj = NULL;
    HRESULT hr = ATL::CComObject<CP2PMessageCom>::CreateInstance ( &pObj );
    if ( FAILED(hr) ) return Fail ( hr, L"CreateMessage" );

    ATL::CComPtr<IUnknown> spHold ( pObj->GetUnknown() );
    hr = pObj->Init ( m_pNet );
    if ( FAILED(hr) ) return Fail ( hr, L"CreateMessage" );

    return pObj->QueryInterface ( IID_IP2PMessageCom, (void**)ppMessage );
}

STDMETHODIMP CP2PNetworkCom::get_VersionString ( BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;
    if ( m_pNet == NULL ) return Fail ( p2pf::P2PF_E_CLOSED, L"VersionString" );

    const wchar_t *wsz = m_pNet->VersionString();
    *pVal = ::SysAllocString ( ( wsz != NULL ) ? wsz : L"" );
    return ( *pVal != NULL ) ? S_OK : Fail ( E_OUTOFMEMORY, L"VersionString" );
}

STDMETHODIMP CP2PNetworkCom::get_MaxPayload ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = (LONG)p2pf::MAX_PAYLOAD;
    return S_OK;
}

// The one verb only the network can offer: it holds every live hub, so it can
// arm the listening side before the dialing one.  A script cannot know that a
// dmx:// or serial:// dial never retries -- this is where that stops mattering.
//
// Straight through to the facade, which does all the work including the
// pre-flight and the unwind.  Nothing is validated twice here: a NULL BSTR
// becomes L"" (which for `endpoint` legitimately means "resolve an in-process
// link"), and every refusal comes back as the facade's own code with a sentence
// attached.
STDMETHODIMP CP2PNetworkCom::Link ( BSTR listenerAddr, BSTR dialerAddr, BSTR endpoint )
{
    if ( m_pNet == NULL )
        return Fail ( p2pf::P2PF_E_CLOSED, L"Link", listenerAddr, dialerAddr, endpoint );

    HRESULT hr = m_pNet->Link ( Str(listenerAddr), Str(dialerAddr), Str(endpoint) );

    // FAILED, never !S_OK: Link answers P2PF_S_UNRELATED_LINK for a sibling
    // pair, and that is a SUCCESS code an early-bound caller is entitled to see.
    return FAILED(hr) ? Fail ( hr, L"Link", listenerAddr, dialerAddr, endpoint ) : hr;
}

STDMETHODIMP CP2PNetworkCom::SetEndpoint ( BSTR address, BSTR endpoint )
{
    if ( m_pNet == NULL )
        return Fail ( p2pf::P2PF_E_CLOSED, L"SetEndpoint", address, endpoint );

    HRESULT hr = m_pNet->SetEndpoint ( Str(address), Str(endpoint) );
    return FAILED(hr) ? Fail ( hr, L"SetEndpoint", address, endpoint ) : hr;
}

//
//  Replace the whole map, and say WHICH LINE if it will not take
//  NOTES: The one place this tier does not simply forward the facade's error.
//         A map is set from a file, and "the endpoint could not be parsed" with
//         no line number sends a script author looking through forty lines by
//         hand -- which is the exact failure the flat ABI's badLine out-param
//         exists to prevent, and an [out] parameter is the one thing a
//         late-bound caller handles worst.  So the number goes in the message
//         instead, where Err.Description already is
//       : The text is NOT echoed back.  Every other call in this layer quotes
//         its arguments, but here the argument is a whole file
//
STDMETHODIMP CP2PNetworkCom::SetEndpointMap ( BSTR text )
{
    if ( m_pNet == NULL )
        return Fail ( p2pf::P2PF_E_CLOSED, L"SetEndpointMap" );

    unsigned int uBadLine = 0;
    HRESULT hr = m_pNet->SetEndpointMap ( Str(text), &uBadLine );
    if ( SUCCEEDED(hr) )
        return hr;

    if ( uBadLine == 0 )
        return Fail ( hr, L"SetEndpointMap" );

    wchar_t wszWhere[32];
    ::swprintf_s ( wszWhere, L"line %u", uBadLine );
    ATL::CComBSTR bsWhere ( wszWhere );
    return Fail ( hr, L"SetEndpointMap", bsWhere );
}

STDMETHODIMP CP2PNetworkCom::EndpointFor ( BSTR address, BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;

    if ( m_pNet == NULL )
        return Fail ( p2pf::P2PF_E_CLOSED, L"EndpointFor", address );

    // "No entry" is an EMPTY STRING here, not an error. P2PF_E_UNRESOLVED is
    // right for the flat ABI, where a caller tests an HRESULT; from a script it
    // would be a raised exception for the entirely ordinary act of asking
    // whether an address is configured. The hub's EndpointFor answers its own
    // "nothing armed" the same way, for the same reason.
    unsigned int cch = 0;
    HRESULT hr = m_pNet->GetEndpointFor ( Str(address), NULL, &cch );
    if ( hr == p2pf::P2PF_E_UNRESOLVED )
    {
        *pVal = ::SysAllocString ( L"" );
        return ( *pVal != NULL ) ? S_OK : E_OUTOFMEMORY;
    }
    if ( FAILED(hr) )
        return Fail ( hr, L"EndpointFor", address );

    ATL::CComBSTR bs;
    bs.Attach ( ::SysAllocStringLen ( NULL, cch > 0 ? cch - 1 : 0 ) );
    if ( !bs )
        return Fail ( E_OUTOFMEMORY, L"EndpointFor", address );

    hr = m_pNet->GetEndpointFor ( Str(address), (wchar_t*)(BSTR)bs, &cch );
    if ( FAILED(hr) )
        return Fail ( hr, L"EndpointFor", address );

    *pVal = bs.Detach();
    return S_OK;
}

void CP2PNetworkCom::ForgetHub ( CP2PHubCom *pHub )
{
    if ( pHub == NULL ) return;

    IUnknown *pUnk   = pHub->GetUnknown();
    IUnknown *pFound = NULL;

    m_csHubs.Lock();
    for ( std::vector<IUnknown*>::iterator it = m_hubs.begin(); it != m_hubs.end(); ++it )
        if ( *it == pUnk ) { pFound = *it; m_hubs.erase ( it ); break; }
    m_csHubs.Unlock();

    if ( pFound != NULL )
        pFound->Release();
}
