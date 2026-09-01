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
// ComMessage.cpp -- IP2PMessageCom over p2pf::IP2PMessage.
#include "stdafx.h"
#include "ComMessage.h"
#include "ComHub.h"          // Str(), ComFail(), VariantToBytes/BytesToVariant

CP2PMessageCom::CP2PMessageCom ( )
    : m_pMsg ( NULL )
{
}

void CP2PMessageCom::FinalRelease ( )
{
    if ( m_pMsg != NULL )
    {
        m_pMsg->Release();
        m_pMsg = NULL;
    }
}

HRESULT CP2PMessageCom::Init ( p2pf::IP2PNetwork *pNet )
{
    if ( pNet == NULL )
        return E_POINTER;
    return pNet->CreateMessage ( &m_pMsg );
}

// One error reporter, so a refused field reads as a sentence rather than a
// hex code -- the same bargain the rest of this layer makes.
static HRESULT Fail ( HRESULT hr, LPCWSTR wszCall, BSTR a1 = NULL )
{
    return ComFail ( IID_IP2PMessageCom, hr, wszCall, a1 );
}

STDMETHODIMP CP2PMessageCom::SetPayload ( VARIANT payload )
{
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    std::vector<BYTE> bytes;
    HRESULT hr = VariantToBytes ( payload, bytes );
    if ( SUCCEEDED(hr) )
        hr = m_pMsg->SetPayload ( bytes.empty() ? NULL : &bytes[0]
                                , (unsigned int)bytes.size() );
    return FAILED(hr) ? Fail ( hr, L"SetPayload" ) : hr;
}

//
// An EMPTY value is legal and means an empty field, which is not the same as
// an absent one -- so VT_EMPTY here must reach the facade as a zero-length
// field rather than be refused. VariantToBytes already answers an empty vector
// for it, so nothing special is needed; this comment is here because it looks
// like an omission.
//
STDMETHODIMP CP2PMessageCom::SetField ( BSTR name, VARIANT value )
{
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    std::vector<BYTE> bytes;
    HRESULT hr = VariantToBytes ( value, bytes );
    if ( SUCCEEDED(hr) )
        hr = m_pMsg->SetField ( Str(name)
                              , bytes.empty() ? NULL : &bytes[0]
                              , (unsigned int)bytes.size() );
    return FAILED(hr) ? Fail ( hr, L"SetField", name ) : hr;
}

STDMETHODIMP CP2PMessageCom::SetFieldText ( BSTR name, BSTR text )
{
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    HRESULT hr = m_pMsg->SetFieldText ( Str(name), Str(text) );
    return FAILED(hr) ? Fail ( hr, L"SetFieldText", name ) : hr;
}

STDMETHODIMP CP2PMessageCom::RemoveField ( BSTR name, VARIANT_BOOL *pRemoved )
{
    if ( pRemoved == NULL ) return E_POINTER;
    *pRemoved = VARIANT_FALSE;
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    HRESULT hr = m_pMsg->RemoveField ( Str(name) );
    if ( FAILED(hr) ) return Fail ( hr, L"RemoveField", name );

    // S_FALSE ("there was no such field") is invisible to an automation
    // client, so it comes back as the return value instead.
    *pRemoved = ( hr == S_OK ) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CP2PMessageCom::get_FieldCount ( LONG *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = 0;
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    unsigned int n = 0;
    HRESULT hr = m_pMsg->GetFieldCount ( &n );
    if ( FAILED(hr) ) return Fail ( hr, L"FieldCount" );
    *pVal = (LONG)n;
    return S_OK;
}

STDMETHODIMP CP2PMessageCom::FieldName ( LONG index, BSTR *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    *pVal = NULL;
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    unsigned int cch = 0;
    HRESULT hr = m_pMsg->GetFieldName ( (unsigned int)index, NULL, &cch );
    if ( FAILED(hr) ) return Fail ( hr, L"FieldName" );

    std::vector<wchar_t> buf ( cch ? cch : 1, L'\0' );
    hr = m_pMsg->GetFieldName ( (unsigned int)index, &buf[0], &cch );
    if ( FAILED(hr) ) return Fail ( hr, L"FieldName" );

    *pVal = ::SysAllocString ( &buf[0] );
    return ( *pVal != NULL ) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CP2PMessageCom::GetField ( BSTR name, VARIANT *pVal )
{
    if ( pVal == NULL ) return E_POINTER;
    ::VariantInit ( pVal );
    if ( m_pMsg == NULL ) return E_UNEXPECTED;

    unsigned int cb = 0;
    HRESULT hr = m_pMsg->GetField ( Str(name), NULL, &cb );
    if ( FAILED(hr) ) return Fail ( hr, L"GetField", name );

    std::vector<BYTE> bytes ( cb );
    if ( cb )
    {
        hr = m_pMsg->GetField ( Str(name), &bytes[0], &cb );
        if ( FAILED(hr) ) return Fail ( hr, L"GetField", name );
    }
    // A present-but-empty field answers an EMPTY ARRAY, not VT_EMPTY: the
    // caller asked for a field that exists, and an array of nothing says so.
    return BytesToVariant ( bytes.empty() ? NULL : &bytes[0], cb, pVal );
}

STDMETHODIMP CP2PMessageCom::Clear ( )
{
    if ( m_pMsg == NULL ) return E_UNEXPECTED;
    HRESULT hr = m_pMsg->Clear();
    return FAILED(hr) ? Fail ( hr, L"Clear" ) : hr;
}
