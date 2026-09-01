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
// FacadeMessage.h -- FacadeMessage, the object behind p2pf::IP2PMessage.
//
// A VALUE, and the header says so at length: no hub, no kernel object, no
// thread affinity, no lifetime rule but Release. All it holds is a payload and
// an ordered list of named blobs.
//
// It deliberately does NOT hold a P2PeerMsg. Building one at fill-in time
// would give this object a kernel lifetime, a hub to belong to, and a reason
// to care which thread touched it -- all three of which are the properties
// that make an IP2PMessage useless as an argument you can send twice. The
// kernel message is built at SEND time, by FacadeHub, out of these bytes.
#pragma once

#include "TargetFacade.h"

#include <string>
#include <vector>

class FacadeMessage : public p2pf::IP2PMessage
{
    // Constructors and destructor
    public:
        FacadeMessage ( );
       ~FacadeMessage ( );

    // p2pf::IP2PMessage
    public:
      virtual HRESULT SetPayload    ( const void *payload, unsigned int size );
      virtual HRESULT SetField      ( const wchar_t *name
                                    , const void *value, unsigned int size );
      virtual HRESULT SetFieldText  ( const wchar_t *name, const wchar_t *text );
      virtual HRESULT RemoveField   ( const wchar_t *name );
      virtual HRESULT GetFieldCount ( unsigned int *outCount ) const;
      virtual HRESULT GetFieldName  ( unsigned int index
                                    , wchar_t *buf, unsigned int *cch ) const;
      virtual HRESULT GetField      ( const wchar_t *name
                                    , void *buf, unsigned int *size ) const;
      virtual HRESULT Clear         ( );
      virtual ULONG   Release       ( );

    // What FacadeHub needs to turn this into a P2PeerMsg
    public:
        // One field, in insertion order.
        struct Field
        {
            std::wstring       strName;
            std::vector<char>  aValue;
        };
        const std::vector<char>&  Payload ( ) const { return m_aPayload; }
        const std::vector<Field>& Fields  ( ) const { return m_aFields;  }

    // Internals
    private:
        // The field of that name, or NULL. Linear because MAX_FIELDS is 64 and
        // insertion order is part of the contract -- a map would cost the
        // ordering to save nothing measurable.
        Field*
          Find ( const wchar_t *name );
        const Field*
          Find ( const wchar_t *name ) const;

    // Attributes
    private:
        std::vector<char>  m_aPayload;
        std::vector<Field> m_aFields;
};
