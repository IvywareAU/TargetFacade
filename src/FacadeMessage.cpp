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
// FacadeMessage.cpp -- IP2PMessage over two std::vectors.
#include "stdafx.h"
#include "FacadeMessage.h"
#include "FacadeInternal.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace {

// "P2PF" is the facade's own reserved prefix, here for the same reason
// "P2Pmsg" is the kernel's on topics: the encoder puts its own bookkeeping in
// this namespace, and a client that could write into it could forge it.
inline bool
IsReservedField ( const wchar_t *lpszName )
{
    return lpszName && ::wcsncmp ( lpszName, L"P2PF", 4 ) == 0;
}

} // namespace

FacadeMessage::FacadeMessage ( )
{
}

FacadeMessage::~FacadeMessage ( )
{
}

FacadeMessage::Field*
FacadeMessage::Find ( const wchar_t *name )
{
    for ( std::vector<Field>::iterator it = m_aFields.begin()
        ; it != m_aFields.end(); ++it )
      if ( it->strName == name )
        return &*it;
    return 0;
}

const FacadeMessage::Field*
FacadeMessage::Find ( const wchar_t *name ) const
{
    return const_cast<FacadeMessage*>(this)->Find ( name );
}

HRESULT
FacadeMessage::SetPayload ( const void *payload, unsigned int size )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( size > p2pf::MAX_PAYLOAD )   return E_INVALIDARG;
    if ( !payload && size )           return E_POINTER;

    m_aPayload.clear();
    if ( size )
      m_aPayload.assign ( (const char*)payload, (const char*)payload + size );
    return S_OK;
}

//
//  Set or replace one named field
//  NOTES: Replacing keeps the field's ORIGINAL position.  Insertion order is
//         part of what GetFieldName(index) promises, and a client that
//         overwrites a value has not re-inserted the field
//
HRESULT
FacadeMessage::SetField ( const wchar_t *name
                        , const void *value, unsigned int size )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !name )                      return E_POINTER;
    if ( !*name )                     return E_INVALIDARG;
    if ( IsReservedField ( name ) )   return p2pf::P2PF_E_RESERVED_TOPIC;
    if ( !value && size )             return E_POINTER;
    if ( ::wcslen ( name ) > p2pf::MAX_FIELD_NAME ||
         size > p2pf::MAX_FIELD_SIZE                )
      return p2pf::P2PF_E_FIELD_LIMIT;

    Field *pField = Find ( name );
    if ( !pField )
    {
      if ( m_aFields.size() >= p2pf::MAX_FIELDS )
        return p2pf::P2PF_E_FIELD_LIMIT;
      m_aFields.push_back ( Field() );
      pField = &m_aFields.back();
      pField->strName = name;
    }

    pField->aValue.clear();
    if ( size )
      pField->aValue.assign ( (const char*)value, (const char*)value + size );
    return S_OK;
}

HRESULT
FacadeMessage::SetFieldText ( const wchar_t *name, const wchar_t *text )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !text ) return E_POINTER;
    // Including the terminator, so the receiver can use the bytes as a string.
    return SetField ( name, text
                    , (unsigned int)( ( ::wcslen(text) + 1 ) * sizeof(wchar_t) ) );
}

HRESULT
FacadeMessage::RemoveField ( const wchar_t *name )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !name ) return E_POINTER;
    for ( std::vector<Field>::iterator it = m_aFields.begin()
        ; it != m_aFields.end(); ++it )
      if ( it->strName == name )
      {
        m_aFields.erase ( it );
        return S_OK;
      }
    return S_FALSE;
}

HRESULT
FacadeMessage::GetFieldCount ( unsigned int *outCount ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !outCount ) return E_POINTER;
    *outCount = (unsigned int)m_aFields.size();
    return S_OK;
}

HRESULT
FacadeMessage::GetFieldName ( unsigned int index
                            , wchar_t *buf, unsigned int *cch ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( index >= m_aFields.size() ) return E_INVALIDARG;
    return CopyOut ( m_aFields[index].strName.c_str(), buf, cch );
}

HRESULT
FacadeMessage::GetField ( const wchar_t *name
                        , void *buf, unsigned int *size ) const
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    if ( !name || !size ) return E_POINTER;

    const Field *pField = Find ( name );
    if ( !pField )
      return p2pf::P2PF_E_NO_FIELD;

    return CopyOutBytes ( pField->aValue.empty() ? 0 : &pField->aValue[0]
                        , (unsigned int)pField->aValue.size(), buf, size );
}

HRESULT
FacadeMessage::Clear ( )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    m_aPayload.clear();
    m_aFields.clear();
    return S_OK;
}

ULONG
FacadeMessage::Release ( )
{
    AFX_MANAGE_STATE ( AfxGetStaticModuleState() );

    delete this;
    return 0;
}
