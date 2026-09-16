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
// TargetFacadeFn.hpp
//
// OPTIONAL header-only convenience layer over TargetFacade.h: std::function
// callbacks and per-topic dispatch, in the style proven by a downstream
// client's PeerNode::onTopic() -- the modern alternative to
// BEGIN_P2PeerMsg_MAP.
//
// Nothing in this file crosses the DLL boundary; it is pure client-side
// sugar around the flat IP2PHubEvents sink, so richer C++ types (std::function,
// std::wstring, lambdas with captures) stay safely inside the client module.
//
// Usage sketch (the whole common case, no macros anywhere):
//
//     p2pf::Network net;                          // one init object
//     p2pf::Hub hub = net.createHub(L"Demo.Server");
//     hub.onTopic(L"chat", [](const p2pf::Message& m) {
//         wprintf(L"%s says: %s\n", m.source, (const wchar_t*)m.payload);
//     });
//     hub.onPeerUp([](const wchar_t* peer) { wprintf(L"up: %s\n", peer); });
//     hub.listen(L"Demo.Client", L"tcp://:7788");
//     ...
//     // everything torn down by destructors, in the right order

#pragma once

#include "TargetFacade.h"

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace p2pf {

// What a topic handler receives.  `payload` is only valid inside the handler.
struct Message
{
    const wchar_t *source    = nullptr;
    const wchar_t *topic     = nullptr;
    const void    *payload   = nullptr;
    unsigned int   size      = 0;
    bool           broadcast = false;

    // --- ABI 7 ------------------------------------------------------------
    // The per-message properties the flat ABI makes you ask for with
    // IP2PHub::GetMsgInfo, inside the callback, on the pump thread.  This
    // layer has already asked -- the whole rule exists because the flat ABI
    // has nowhere to put them, and here there is somewhere.
    //
    // `tag` is the correlation token the sender passed to sendEx/broadcastEx,
    // 0 when it set none.  Answer with Hub::reply and it goes straight back.
    unsigned int   tag       = 0;
    unsigned int   priority  = P2PF_PRI_NORMAL;
    unsigned int   flags     = 0;       // P2PF_MSG_*

    // Payload-as-text view; valid only if the sender used sendText().
    const wchar_t* text ( ) const { return (const wchar_t*)payload; }

    // True when the sender left bounce reporting on -- i.e. declining this
    // message (returning S_FALSE from onFilter) will actually tell them.
    bool bounces ( ) const { return ( flags & P2PF_MSG_BOUNCES ) != 0; }

    // The address this copy was addressed TO.  A method rather than a field
    // because it is the one field that costs a string copy, and because it is
    // this hub's own address on very nearly every message -- see
    // IP2PHub::GetMsgInfo.  Empty outside a handler.
    inline std::wstring destination ( ) const;

    // The hub delivering this message, for destination() and reply().
    IP2PHub       *hub       = nullptr;

    // --- ABI 8: named fields ----------------------------------------------
    // Valid for the length of the handler, like `payload`.

    // True when this message carries any fields at all.
    bool hasFields ( ) const { return ( flags & P2PF_MSG_FIELDS ) != 0; }

    // One field's bytes.  Empty when there is no such field -- use has() to
    // tell an absent field from one that is present and empty, because that
    // distinction is a signal the ABI deliberately keeps.
    inline std::vector<unsigned char> field ( const wchar_t *name ) const;

    // One field as text, for a sender that used setFieldText.
    inline std::wstring fieldText ( const wchar_t *name ) const;

    // Is this field present at all?
    inline bool has ( const wchar_t *name ) const;

    // Every field name, in the order the sender set them.
    inline std::vector<std::wstring> fieldNames ( ) const;
};

inline std::wstring Message::destination ( ) const
{
    unsigned int cch = 0;
    if ( !hub || FAILED ( hub->GetMsgInfo ( nullptr, &cch, nullptr, nullptr,
                                            nullptr ) ) || cch < 2 )
        return std::wstring();
    std::wstring s ( cch - 1, L'\0' );
    if ( FAILED ( hub->GetMsgInfo ( &s[0], &cch, nullptr, nullptr, nullptr ) ) )
        return std::wstring();
    return s;
}

inline bool Message::has ( const wchar_t *name ) const
{
    unsigned int cb = 0;
    return hub && SUCCEEDED ( hub->GetField ( name, nullptr, &cb ) );
}

inline std::vector<unsigned char> Message::field ( const wchar_t *name ) const
{
    std::vector<unsigned char> out;
    unsigned int cb = 0;
    if ( !hub || FAILED ( hub->GetField ( name, nullptr, &cb ) ) || cb == 0 )
        return out;
    out.resize ( cb );
    if ( FAILED ( hub->GetField ( name, &out[0], &cb ) ) )
        out.clear();
    return out;
}

inline std::wstring Message::fieldText ( const wchar_t *name ) const
{
    std::vector<unsigned char> v = field ( name );
    if ( v.size() < sizeof(wchar_t) )
        return std::wstring();
    // Stored with its terminator by setFieldText; drop it rather than hand
    // back a string whose length counts a NUL.
    return std::wstring ( (const wchar_t*)&v[0], v.size() / sizeof(wchar_t) - 1 );
}

inline std::vector<std::wstring> Message::fieldNames ( ) const
{
    std::vector<std::wstring> out;
    unsigned int n = 0;
    if ( !hub || FAILED ( hub->GetFieldCount ( &n ) ) )
        return out;
    out.reserve ( n );
    for ( unsigned int i = 0; i < n; ++i )
    {
        unsigned int cch = 0;
        if ( FAILED ( hub->GetFieldName ( i, nullptr, &cch ) ) || cch < 2 )
            continue;
        std::wstring s ( cch - 1, L'\0' );
        if ( SUCCEEDED ( hub->GetFieldName ( i, &s[0], &cch ) ) )
            out.push_back ( s );
    }
    return out;
}

class Hub;

// ---------------------------------------------------------------------------
// Message -- RAII around IP2PMessage, for the SEND side.
//
// The flat object is a value with a Release; this is the same value with a
// destructor.  Everything about it that matters is in IP2PMessage: no hub, no
// thread rule, not consumed by sending.
// ---------------------------------------------------------------------------
class OutMessage
{
  public:
    explicit OutMessage ( IP2PMessage *p ) : m_p ( p ) { }
   ~OutMessage ( )                        { if ( m_p ) m_p->Release(); }

    OutMessage             ( const OutMessage& ) = delete;
    OutMessage& operator = ( const OutMessage& ) = delete;
    OutMessage ( OutMessage&& rhs ) noexcept : m_p ( rhs.m_p ) { rhs.m_p = nullptr; }

    HRESULT payload ( const void *data, unsigned int size )
                                      { return m_p->SetPayload ( data, size ); }
    HRESULT set     ( const wchar_t *name, const void *value, unsigned int size )
                                      { return m_p->SetField ( name, value, size ); }
    HRESULT setText ( const wchar_t *name, const wchar_t *text )
                                      { return m_p->SetFieldText ( name, text ); }
    HRESULT remove  ( const wchar_t *name )
                                      { return m_p->RemoveField ( name ); }
    HRESULT clear   ( )               { return m_p->Clear(); }

    unsigned int count ( ) const
    {
        unsigned int n = 0;
        if ( m_p ) m_p->GetFieldCount ( &n );
        return n;
    }

    IP2PMessage* raw ( ) const        { return m_p; }
    explicit operator bool ( ) const  { return m_p != nullptr; }

  private:
    IP2PMessage *m_p = nullptr;
};

// ---------------------------------------------------------------------------
// DiagEvent -- one line of the kernel's narration, as handed to onDiag.
//                                                                    (ABI 10)
// The four things a log line always wants are already here; everything else is
// a method that asks the facade, so an event nobody inspects costs four
// pointers.  All of it is valid ONLY inside the handler -- which is the whole
// reason this is a borrowed object and not a struct you can copy.
//
// Read IP2PDiagEvents and "Diagnostics" in TargetFacade.h before writing one:
// the handler runs on the thread that RAISED the event, and can run on several
// at once.
// ---------------------------------------------------------------------------
class DiagEvent
{
  public:
    DiagEvent ( IP2PNetwork *net, unsigned int sev, unsigned int no
              , const wchar_t *mod, const wchar_t *txt )
      : m_net ( net ), m_severity ( sev ), m_eventNo ( no )
      , m_module ( mod ? mod : L"" ), m_text ( txt ? txt : L"" ) { }

    DiagEvent             ( const DiagEvent& ) = delete;
    DiagEvent& operator = ( const DiagEvent& ) = delete;

    unsigned int   severity ( ) const { return m_severity; }
    unsigned int   eventNo  ( ) const { return m_eventNo;  }
    const wchar_t* module   ( ) const { return m_module;   }
    const wchar_t* text     ( ) const { return m_text;     }

    bool isError   ( ) const          { return m_severity == P2PF_DIAG_ERROR; }
    bool isWarning ( ) const          { return m_severity == P2PF_DIAG_WARNING; }
    bool isProblem ( ) const          { return isError() || isWarning(); }

    // The parts with no argument on the callback.  Empty when the raiser
    // attached none -- which is the common case for all but `className`.
    std::wstring advice      ( ) const { return part ( P2PF_DIAGT_ADVICE  ); }
    std::wstring group       ( ) const { return part ( P2PF_DIAGT_GROUP   ); }
    std::wstring service     ( ) const { return part ( P2PF_DIAGT_SERVICE ); }
    std::wstring className   ( ) const { return part ( P2PF_DIAGT_CLASS   ); }
    std::wstring hresultText ( ) const { return part ( P2PF_DIAGT_HRESULT ); }

    unsigned int hresult  ( ) const   { return num ( 0 ); }
    unsigned int time     ( ) const   { return num ( 1 ); }
    unsigned int threadId ( ) const   { return num ( 2 ); }

  private:
    std::wstring part ( unsigned int which ) const
    {
        unsigned int cch = 0;
        if ( !m_net || FAILED ( m_net->GetDiagText ( which, nullptr, &cch ) ) || cch < 2 )
            return std::wstring();
        std::wstring s ( cch - 1, L'\0' );
        if ( FAILED ( m_net->GetDiagText ( which, &s[0], &cch ) ) )
            return std::wstring();
        return s;
    }
    unsigned int num ( int which ) const
    {
        unsigned int hr = 0, t = 0, tid = 0;
        if ( !m_net || FAILED ( m_net->GetDiagInfo ( &hr, &t, &tid ) ) )
            return 0;
        return which == 0 ? hr : which == 1 ? t : tid;
    }

    IP2PNetwork   *m_net;
    unsigned int   m_severity;
    unsigned int   m_eventNo;
    const wchar_t *m_module;
    const wchar_t *m_text;
};

// ---------------------------------------------------------------------------
// Network -- RAII around P2PF_CreateNetwork/Release.
// ---------------------------------------------------------------------------
class Network
{
  public:
    Network ( )
    {
        HRESULT hr = P2PF_CreateNetwork ( ABI_VERSION, &m_pNet );
        if ( FAILED(hr) )
            throw std::runtime_error ( "P2PF_CreateNetwork failed" );
    }
   ~Network ( )
    {
        // The sink FIRST and by hand: it is about to be destroyed with the
        // rest of this object, and the kernel's slot holds a raw pointer to
        // it.  Release() would do this too, but only if this really is the
        // last reference -- and a second Network in the process would go on
        // delivering into freed memory.
        if ( m_pNet && m_diag ) m_pNet->SetDiagSink ( nullptr, 0 );
        if ( m_pNet ) m_pNet->Release();
    }

    Network             ( const Network& ) = delete;
    Network& operator = ( const Network& ) = delete;

    // `flags` is P2PF_HUB_SPAWN_PUMP (the default -- the hub gets a thread of
    // its own) or P2PF_HUB_CALLER_PUMPED, in which case the hub runs on THIS
    // thread and does nothing until this thread calls Hub::pump or Hub::run.
    // OR in P2PF_HUB_SECURE for a hub that holds an identity and demands a
    // signed login from every peer it links to -- see IP2PHub::GetSecurityInfo
    // for the whole of what that arranges, and Hub::securityFlags to read it
    // back.  (ABI 11)
    inline Hub createHub ( const wchar_t *address
                         , unsigned int flags = P2PF_HUB_SPAWN_PUMP ); // below

    // An empty message to fill in and hand to Hub::sendMsg.  On the NETWORK
    // because it belongs to no hub -- see IP2PMessage.
    OutMessage createMessage ( )
    {
        IP2PMessage *p = nullptr;
        if ( FAILED ( m_pNet->CreateMessage ( &p ) ) )
            throw std::runtime_error ( "IP2PNetwork::CreateMessage failed" );
        return OutMessage ( p );
    }

    // Arm both ends of one edge between two hubs of this network, listening
    // side first.  `endpoint` omitted = an in-process Dmx link with no
    // configuration at all.  See IP2PNetwork::Link -- the argument order is
    // listener, then dialer, and it is not symmetric.
    HRESULT link ( const wchar_t *listenerAddr, const wchar_t *dialerAddr
                 , const wchar_t *endpoint = nullptr )
                                      { return m_pNet->Link ( listenerAddr, dialerAddr, endpoint ); }

    // Where every SECURE hub of this process keeps its key material.
    // Omitted/empty = a "p2p" directory beside the loaded module.  Call it
    // before the first createHub(addr, P2PF_HUB_SECURE).  (ABI 11)
    HRESULT setSecurityDir ( const wchar_t *dir = nullptr )
                                      { return m_pNet->SetSecurityDir ( dir ); }

    // The deployment map: where each ADDRESS lives, in dial form.  An omitted
    // endpoint on listen()/connect() consults this before it falls back on the
    // in-process convention, so a whole topology can come from a file and no
    // endpoint need appear in the code at all.  See IP2PNetwork.
    HRESULT setEndpoint ( const wchar_t *address, const wchar_t *endpoint )
                                      { return m_pNet->SetEndpoint ( address, endpoint ); }

    // Replaces the map.  Empty text clears it.  On failure nothing is applied
    // and `badLine` (if given) receives the 1-based offending line number.
    HRESULT setEndpointMap ( const wchar_t *text, unsigned int *badLine = nullptr )
                                      { return m_pNet->SetEndpointMap ( text, badLine ); }

    // Convenience for the common shape -- a whole ini in one std::wstring.
    HRESULT setEndpointMap ( const std::wstring& text, unsigned int *badLine = nullptr )
                                      { return m_pNet->SetEndpointMap ( text.c_str(), badLine ); }

    // What the MAP says about `address` (not what a hub armed -- that is
    // Hub::endpointFor).  Empty when there is no entry.
    std::wstring endpointFor ( const wchar_t *address ) const
    {
        unsigned int cch = 0;
        if ( !m_pNet || FAILED ( m_pNet->GetEndpointFor ( address, nullptr, &cch ) ) || cch < 2 )
            return std::wstring();
        std::wstring s ( cch - 1, L'\0' );
        if ( FAILED ( m_pNet->GetEndpointFor ( address, &s[0], &cch ) ) )
            return std::wstring();
        return s;
    }

    // --- diagnostics ---------------------------------------------------------
    //
    // Subscribe to the kernel's own narration.  One handler per process; a
    // second call replaces the first, and onDiag(nullptr) unsubscribes.
    //
    //     net.onDiag ( [](const p2pf::DiagEvent& d) {
    //         wprintf ( L"[%u] %s: %s\n", d.threadId(), d.module(), d.text() );
    //     }, p2pf::P2PF_DIAGM_PROBLEMS );
    //
    // THE HANDLER RUNS ON THE THREAD THAT RAISED THE EVENT, and on several
    // threads at once -- it is the one callback in this header that is not
    // serialised by a pump.  Capture nothing you are not prepared to lock.
    // See "Diagnostics" in TargetFacade.h; the mask defaults to errors and
    // warnings because P2PF_DIAGM_ALL is a firehose in a Debug build.
    using DiagHandler = std::function<void(const DiagEvent&)>;

    HRESULT onDiag ( DiagHandler fn
                   , unsigned int mask = P2PF_DIAGM_PROBLEMS )
    {
        std::unique_ptr<FnDiagSink> next;
        if ( fn )
            next.reset ( new FnDiagSink ( this, std::move(fn) ) );

        // Built before it is registered, and registered before the old one is
        // let go: SetDiagSink waits for in-flight deliveries to leave the sink
        // it is replacing, so once it returns the old object has nobody in it.
        HRESULT hr = m_pNet->SetDiagSink ( next.get(), next ? mask : 0 );
        if ( FAILED(hr) )
            return hr;

        retire ( std::move ( m_diag ) );
        m_diag = std::move ( next );
        return hr;
    }

    // Narrow or widen without replacing the handler.  S_FALSE when none is
    // registered, which is not a failure -- see IP2PNetwork::SetDiagMask.
    HRESULT diagMask ( unsigned int mask )
                                      { return m_pNet->SetDiagMask ( mask ); }
    unsigned int diagMask ( ) const
    {
        unsigned int m = 0;
        if ( m_pNet ) m_pNet->GetDiagMask ( &m );
        return m;
    }

    // Write one line into the same stream.  S_FALSE means it was suppressed
    // because this thread is inside a diagnostics handler.
    HRESULT raiseDiag ( unsigned int severity, const wchar_t *module
                      , const wchar_t *text )
                                      { return m_pNet->RaiseDiag ( severity, module, text ); }
    HRESULT log   ( const wchar_t *module, const wchar_t *text )
                                      { return raiseDiag ( P2PF_DIAG_APP, module, text ); }

    // Is anything listening for these classes?  Ask before building an
    // expensive line -- see IP2PNetwork::IsDiagWanted.
    bool diagWanted ( unsigned int mask ) const
    {
        unsigned int matched = 0;
        return m_pNet
            && SUCCEEDED ( m_pNet->IsDiagWanted ( mask, &matched ) )
            && matched != 0;
    }

    IP2PNetwork* raw ( ) const        { return m_pNet; }

  private:
    // The flat sink, kept alive by the Network for exactly as long as it is
    // registered.  It holds the Network so DiagEvent can ask it for the parts
    // the callback has no argument for.
    struct FnDiagSink : public IP2PDiagEvents
    {
        FnDiagSink ( Network *owner, DiagHandler fn )
          : m_owner ( owner ), m_fn ( std::move(fn) ) { }

        virtual void OnDiag ( unsigned int severity, unsigned int eventNo
                            , const wchar_t *module, const wchar_t *text )
        {
            if ( !m_fn ) return;
            DiagEvent d ( m_owner->raw(), severity, eventNo, module, text );
            m_fn ( d );
        }

        Network     *m_owner;
        DiagHandler  m_fn;
    };

    // Let go of a sink that has just been replaced -- UNLESS this thread is
    // inside a delivery, in which case the object being let go may be the one
    // whose handler is running, three frames up, holding captures it is about
    // to touch again.  `net.onDiag(nullptr)` from inside your own handler is a
    // perfectly reasonable thing to write (a log view closing itself on a
    // fatal line), and it must not free the lambda it is standing on.
    //
    // The test for "am I inside one" is the ABI's own: GetDiagInfo answers
    // P2PF_E_NO_DIAG anywhere else, which is exactly this question.  Retired
    // sinks are freed on the next call made from outside a delivery, and at
    // the latest when the Network goes.
    void retire ( std::unique_ptr<FnDiagSink> old )
    {
        bool bInside = m_pNet
                    && m_pNet->GetDiagInfo ( nullptr, nullptr, nullptr ) == S_OK;
        if ( old )
        {
            if ( bInside ) m_retired.push_back ( std::move ( old ) );
            else           old.reset();
        }
        if ( !bInside )
            m_retired.clear();
    }

    IP2PNetwork                 *m_pNet = nullptr;
    std::unique_ptr<FnDiagSink>  m_diag;
    std::vector<std::unique_ptr<FnDiagSink>> m_retired;
};

// ---------------------------------------------------------------------------
// Hub -- RAII + std::function dispatch around IP2PHub/IP2PHubEvents.
//
// Register handlers BEFORE arming connections: the registries are written on
// the client thread and read on the pump thread, with registration-then-read
// discipline (same single-writer rule PeerNode uses).  Re-registering after
// traffic started is not synchronised -- by design, to stay lock-free on the
// hot path.
// ---------------------------------------------------------------------------
class Hub
{
  public:
    using MessageHandler = std::function<void(const Message&)>;
    using PeerHandler    = std::function<void(const wchar_t* peer)>;
    using ErrorHandler   = std::function<void(const wchar_t* what)>;
    // ABI 6.  A filter is a message handler that ANSWERS: S_OK == handled,
    // S_FALSE == not mine, let the kernel's own handler chain see it.
    using MessageFilter  = std::function<HRESULT(const Message&)>;
    using EventHandler   = std::function<void(unsigned int code,
                                              const wchar_t* peer,
                                              const wchar_t* what)>;
    using TimerHandler   = std::function<void(unsigned int timerId,
                                              unsigned int key)>;
    using PostHandler    = std::function<void(unsigned int key, void* context)>;

    Hub ( ) = default;
    Hub ( Hub&& rhs ) noexcept        { swap ( rhs ); }
    Hub& operator = ( Hub&& rhs ) noexcept { close(); swap ( rhs ); return *this; }
   ~Hub ( )                           { close(); }

    Hub             ( const Hub& ) = delete;
    Hub& operator = ( const Hub& ) = delete;

    // --- handler registration (the no-macro message map) -------------------
    Hub& onTopic   ( const std::wstring& topic, MessageHandler h )
                                      { m_sink->m_topics[topic] = std::move(h); return *this; }
    Hub& onMessage ( MessageHandler h ) { m_sink->m_fallback  = std::move(h); return *this; }
    Hub& onPeerUp  ( PeerHandler h )    { m_sink->m_peerUp    = std::move(h); return *this; }
    Hub& onPeerDown( PeerHandler h )    { m_sink->m_peerDown  = std::move(h); return *this; }
    Hub& onError   ( ErrorHandler h )   { m_sink->m_error     = std::move(h); return *this; }

    // --- ABI 6 handlers ----------------------------------------------------
    // Answer for EVERY message, ahead of the topic registry.  Return S_FALSE
    // and the SENDER is told: it gets onEvent(P2PF_EVT_ROUTING_ERROR) carrying
    // the kernel's own "not handled" sentence.  So decline what is not yours,
    // not what you merely have nothing to do about.
    Hub& onMessageEx ( MessageFilter h ) { m_sink->m_filter = std::move(h); return *this; }
    // The structured half of onError: a P2PF_EVT_* code and the peer it is
    // about.  Registering this REPLACES onError (the facade delivers one or
    // the other, never both).
    Hub& onEvent   ( EventHandler h )   { m_sink->m_event     = std::move(h); return *this; }
    // Fired on the pump thread by setTimer().
    Hub& onTimer   ( TimerHandler h )   { m_sink->m_timer     = std::move(h); return *this; }
    // Fired on the pump thread by post().
    Hub& onPost    ( PostHandler h )    { m_sink->m_post      = std::move(h); return *this; }

    // --- thin forwards to IP2PHub -----------------------------------------
    // One pair of verbs, every transport; the endpoint is a string, so a call
    // site can take it from a config file without changing shape:
    //     hub.listen  ( L"Demo.Client", L"tcp://:7788"          );
    //     hub.connect ( L"Demo.Server", L"tcp://127.0.0.1:7788" );
    //     hub.connect ( L"Demo.Server", L"pipe://demo"          );
    //     hub.listen  ( L"Demo.Client", nullptr );   // in-process, resolved
    HRESULT listen        ( const wchar_t *toPeer, const wchar_t *endpoint )
                                      { return m_pHub->Listen ( toPeer, endpoint ); }
    HRESULT connect       ( const wchar_t *toPeer, const wchar_t *endpoint )
                                      { return m_pHub->Connect ( toPeer, endpoint ); }

    HRESULT send      ( const wchar_t *dest, const wchar_t *topic
                      , const void *payload, unsigned int size )
                                      { return m_pHub->Send ( dest, topic, payload, size ); }
    HRESULT sendText  ( const wchar_t *dest, const wchar_t *topic, const wchar_t *text )
                                      { return m_pHub->SendText ( dest, topic, text ); }
    HRESULT broadcast ( const wchar_t *topic, const void *payload, unsigned int size )
                                      { return m_pHub->Broadcast ( topic, payload, size ); }

    // --- ABI 7: the message model -----------------------------------------
    // Same verbs, saying the three things the plain ones cannot: where this
    // message sits in the pump's queue, an opaque 32-bit correlation `tag`
    // that travels with it and comes back as Message::tag, and P2PF_SEND_*
    // flags.  Defaults make each of these identical to its plain twin.
    HRESULT sendEx      ( const wchar_t *dest, const wchar_t *topic
                        , const void *payload, unsigned int size
                        , unsigned int tag      = 0
                        , unsigned int priority = P2PF_PRI_DEFAULT
                        , unsigned int flags    = 0 )
                                      { return m_pHub->SendEx ( dest, topic, payload, size
                                                              , priority, tag, flags ); }

    HRESULT sendTextEx  ( const wchar_t *dest, const wchar_t *topic
                        , const wchar_t *text
                        , unsigned int tag      = 0
                        , unsigned int priority = P2PF_PRI_DEFAULT
                        , unsigned int flags    = 0 )
    {
        return sendEx ( dest, topic, text
                      , (unsigned int)( ( ::wcslen(text) + 1 ) * sizeof(wchar_t) )
                      , tag, priority, flags );
    }

    HRESULT broadcastEx ( const wchar_t *topic
                        , const void *payload, unsigned int size
                        , unsigned int tag      = 0
                        , unsigned int priority = P2PF_PRI_DEFAULT
                        , unsigned int flags    = 0 )
                                      { return m_pHub->BroadcastEx ( topic, payload, size
                                                                   , priority, tag, flags ); }

    // ANSWER one message: back to its sender, carrying its tag.  This is the
    // whole of request/response on this facade, and it is why the tag exists --
    // missing.md §4.2 named "no request/response correlation" as the sharpest
    // practical consequence of the message model being closed.
    //
    //     hub.onTopic ( L"sum", [&](const Message& m)
    //                   { int r = work(m); hub.reply ( m, L"sum.ok", &r, sizeof(r) ); } );
    //
    // Callable from anywhere, not just inside the handler -- it reads nothing
    // from the hub, only from the Message the caller still holds.  Copy the
    // source and tag out first if you answer later, since `source` points into
    // the callback's storage.
    HRESULT reply ( const Message& m, const wchar_t *topic
                  , const void *payload, unsigned int size
                  , unsigned int flags = 0 )
                                      { return sendEx ( m.source, topic, payload, size
                                                      , m.tag, P2PF_PRI_DEFAULT, flags ); }

    HRESULT replyText ( const Message& m, const wchar_t *topic
                      , const wchar_t *text, unsigned int flags = 0 )
                                      { return sendTextEx ( m.source, topic, text
                                                          , m.tag, P2PF_PRI_DEFAULT, flags ); }

    // --- ABI 8: send a message with named fields ---------------------------
    // `msg` is untouched -- send the same one to twenty peers if you like.
    HRESULT sendMsg      ( const wchar_t *dest, const wchar_t *topic
                         , const OutMessage& msg
                         , unsigned int tag      = 0
                         , unsigned int priority = P2PF_PRI_DEFAULT
                         , unsigned int flags    = 0 )
                                      { return m_pHub->SendMsg ( dest, topic, msg.raw()
                                                               , priority, tag, flags ); }

    HRESULT broadcastMsg ( const wchar_t *topic, const OutMessage& msg
                         , unsigned int tag      = 0
                         , unsigned int priority = P2PF_PRI_DEFAULT
                         , unsigned int flags    = 0 )
                                      { return m_pHub->BroadcastMsg ( topic, msg.raw()
                                                                    , priority, tag, flags ); }

    // Answer one message with another that carries fields, tag and all.
    HRESULT replyMsg ( const Message& m, const wchar_t *topic
                     , const OutMessage& msg, unsigned int flags = 0 )
                                      { return sendMsg ( m.source, topic, msg
                                                       , m.tag, P2PF_PRI_DEFAULT, flags ); }

    const wchar_t* address ( ) const  { return m_pHub ? m_pHub->Address() : L""; }
    bool  isPeerUp ( const wchar_t *peer ) const
                                      { return m_pHub && m_pHub->IsPeerUp ( peer ); }

    // --- read side --------------------------------------------------------
    // The flat surface hands strings back through a caller-sized buffer,
    // because that is what crosses an ABI cleanly.  On this side of the
    // boundary there is no reason for a caller to see that at all.

    // One peer, as this hub knows it.
    struct Con
    {
        std::wstring peer;
        std::wstring endpoint;    // empty: this hub did not arm this peer
        unsigned int flags = 0;   // P2PF_CON_* | exactly one P2PF_REL_*

        bool listening ( ) const { return ( flags & P2PF_CON_LISTEN ) != 0; }
        bool dialing   ( ) const { return ( flags & P2PF_CON_DIAL   ) != 0; }
        bool up        ( ) const { return ( flags & P2PF_CON_UP     ) != 0; }
        // True when this peer is neither above nor below us in the address
        // tree -- the link works point to point but can never be routed
        // through.  See IP2PHub "Topology".
        bool unrelated ( ) const { return ( flags & P2PF_REL_UNRELATED ) != 0; }
    };

    unsigned int conCount ( ) const
    {
        unsigned int n = 0;
        if ( m_pHub ) m_pHub->GetConCount ( &n );
        return n;
    }

    std::vector<Con> cons ( ) const
    {
        std::vector<Con> out;
        if ( !m_pHub ) return out;

        unsigned int n = conCount();
        out.reserve ( n );
        for ( unsigned int i = 0; i < n; ++i )
        {
            unsigned int cchPeer = 0, cchEp = 0, flags = 0;
            if ( FAILED ( m_pHub->GetCon ( i, nullptr, &cchPeer,
                                              nullptr, &cchEp, &flags ) ) )
                break;                                  // the set shrank under us

            Con c;
            c.flags = flags;
            c.peer    .resize ( cchPeer ? cchPeer - 1 : 0 );
            c.endpoint.resize ( cchEp   ? cchEp   - 1 : 0 );
            // &s[0] is contiguous and writable since C++11; the extra slot the
            // facade wants for its terminator is the one std::wstring already
            // keeps past size().
            if ( FAILED ( m_pHub->GetCon ( i,
                          cchPeer ? &c.peer[0]     : nullptr, &cchPeer,
                          cchEp   ? &c.endpoint[0] : nullptr, &cchEp, &flags ) ) )
                break;
            out.push_back ( std::move ( c ) );
        }
        return out;
    }

    // Empty for a peer this hub did not arm, and for one it has never heard of
    // -- tell them apart with cons() if the difference matters.
    std::wstring endpointFor ( const wchar_t *peer ) const
    {
        unsigned int cch = 0;
        if ( !m_pHub || FAILED ( m_pHub->GetEndpoint ( peer, nullptr, &cch ) ) || cch < 2 )
            return std::wstring();
        std::wstring s ( cch - 1, L'\0' );
        if ( FAILED ( m_pHub->GetEndpoint ( peer, &s[0], &cch ) ) )
            return std::wstring();
        return s;
    }

    // The whole hub in one string: address, then a line per peer. Made for
    // logs and bug reports.
    std::wstring describe ( ) const
    {
        unsigned int cch = 0;
        if ( !m_pHub || FAILED ( m_pHub->Describe ( nullptr, &cch ) ) || cch < 2 )
            return std::wstring();
        std::wstring s ( cch - 1, L'\0' );
        if ( FAILED ( m_pHub->Describe ( &s[0], &cch ) ) )
            return std::wstring();
        return s;
    }

    // --- ABI 6: past the messaging slice -----------------------------------

    // Drop one peer and keep the hub. S_FALSE = signalled but still retiring.
    HRESULT disconnect ( const wchar_t *peer )
                                      { return m_pHub->Disconnect ( peer ); }

    // One-shot timer on the PUMP thread -> onTimer(id, key). Never early; up
    // to about a second late (the kernel's poll granularity). Housekeeping and
    // supervision, not pacing.
    HRESULT setTimer ( unsigned int delayMillisecs, unsigned int key
                     , unsigned int *outTimerId )
                                      { return m_pHub->SetTimer ( delayMillisecs, key, outTimerId ); }
    // Same, for call sites that just want the id (0 == it was not armed).
    unsigned int setTimer ( unsigned int delayMillisecs, unsigned int key )
    {
        unsigned int id = 0;
        if ( m_pHub ) m_pHub->SetTimer ( delayMillisecs, key, &id );
        return id;
    }
    HRESULT killTimer ( unsigned int timerId )
                                      { return m_pHub->KillTimer ( timerId ); }

    // Run onPost(key, context) on the pump thread, in FIFO order with the
    // hub's own traffic. The way to hand work to a hub without a lock.
    HRESULT post ( unsigned int key, void *context = nullptr )
                                      { return m_pHub->Post ( key, context ); }

    // Per-connection knobs (P2PF_OPT_*).
    HRESULT setConOption ( const wchar_t *peer, unsigned int option, unsigned int value )
                                      { return m_pHub->SetConOption ( peer, option, value ); }
    HRESULT getConOption ( const wchar_t *peer, unsigned int option, unsigned int *outValue ) const
                                      { return m_pHub->GetConOption ( peer, option, outValue ); }
    // Convenience readers: the answer, or `def` when the peer/option cannot be
    // read at all -- for the call sites that want a value, not a code.
    unsigned int conOption ( const wchar_t *peer, unsigned int option
                           , unsigned int def = 0 ) const
    {
        unsigned int v = def;
        if ( !m_pHub || FAILED ( m_pHub->GetConOption ( peer, option, &v ) ) )
            return def;
        return v;
    }
    bool conEncrypted ( const wchar_t *peer ) const
                                      { return conOption ( peer, P2PF_OPT_ENCRYPTED ) != 0; }
    HRESULT setConTrace ( const wchar_t *peer, bool on )
                                      { return setConOption ( peer, P2PF_OPT_TRACE, on ? 1u : 0u ); }

    // Round trip to a peer, in milliseconds. Facade-to-facade; a peer that is
    // not a facade hub times out. Never call from a handler ON A SPAWNED HUB
    // -- it would block the only thread that could answer, and says so with
    // P2PF_E_PUMP_THREAD. On a caller-pumped hub it is legal there, because
    // the wait drives the pump instead of blocking it (see IP2PHub::Pump).
    HRESULT ping ( const wchar_t *peer, unsigned int timeoutMillisecs
                 , unsigned int *outMillisecs )
                                      { return m_pHub->Ping ( peer, timeoutMillisecs, outMillisecs ); }
    // Same, as a number: -1 when there was no answer.
    int pingMillisecs ( const wchar_t *peer, unsigned int timeoutMillisecs = 0 )
    {
        unsigned int ms = 0;
        if ( !m_pHub || FAILED ( m_pHub->Ping ( peer, timeoutMillisecs, &ms ) ) )
            return -1;
        return (int)ms;
    }

    HRESULT closeIdleCons ( )         { return m_pHub->CloseIdleCons(); }

    // --- security (ABI 11) -------------------------------------------------
    //
    // Both answer for a PLAIN hub too, and the answers are 0 and empty: a hub
    // not created with P2PF_HUB_SECURE holds no identity and enforces nothing.

    // This hub's posture, as P2PF_SEC_* bits read back from the kernel.  The
    // one worth testing is P2PF_SEC_ARMED, which is not implied by
    // P2PF_SEC_REQUIRED -- see IP2PHub::GetSecurityInfo.
    unsigned int securityFlags ( ) const
    {
        unsigned int flags = 0;
        return ( m_pHub &&
                 SUCCEEDED ( m_pHub->GetSecurityInfo ( nullptr, nullptr, &flags ) ) )
             ? flags : 0u;
    }
    bool isSecure ( ) const           { return ( securityFlags() & P2PF_SEC_CAN_SIGN ) != 0; }

    // This hub's identity FINGERPRINT, for a human to compare -- and never an
    // identifier this code trusts.  Empty for a plain hub.
    std::wstring securityFingerprint ( ) const
    {
        unsigned int cch = 0;
        if ( !m_pHub ||
             FAILED ( m_pHub->GetSecurityInfo ( nullptr, &cch, nullptr ) ) || cch <= 1 )
            return std::wstring();
        std::wstring s ( cch, L'\0' );
        if ( FAILED ( m_pHub->GetSecurityInfo ( &s[0], &cch, nullptr ) ) )
            return std::wstring();
        s.resize ( cch ? cch - 1 : 0 );
        return s;
    }

    // --- driving the pump yourself (ABI 9) ---------------------------------
    //
    // Only on a hub made with createHub(addr, P2PF_HUB_CALLER_PUMPED), and
    // only from the thread that made it.  See IP2PHub::Pump.

    // One turn.  S_OK dispatched something, S_FALSE idle, a failure means STOP.
    HRESULT pump ( unsigned int timeoutMillisecs = 0
                 , unsigned int *outWhat = nullptr )
                                      { return m_pHub->Pump ( timeoutMillisecs, outWhat ); }

    // Drain everything that is ready and return WITHOUT blocking.  This is the
    // shape a GUI idle handler or WM_TIMER wants: it never parks the loop, and
    // it is bounded so one very busy hub cannot hold the UI thread for ever.
    // Returns the number of turns that did work.
    unsigned int pumpReady ( unsigned int maxTurns = 64 )
    {
        unsigned int n = 0;
        for ( unsigned int i = 0; i < maxTurns; ++i )
            if ( m_pHub->Pump ( 0, nullptr ) != S_OK )
                break;
            else
                ++n;
        return n;
    }

    // Run the hub on this thread until `pred` says stop or the hub closes.
    // The whole of a console/worker main loop, and the shape the tests use:
    //
    //     hub.run ( [&]{ return done; }, 25 );
    //
    // `sliceMillisecs` is how long one idle turn may park for -- it bounds how
    // late `pred` can be noticed, so keep it small when the predicate is what
    // ends the program.  Returns false if the hub stopped on its own.
    template <typename Pred>
    bool run ( Pred pred, unsigned int sliceMillisecs = 25 )
    {
        while ( !pred() )
            if ( FAILED ( m_pHub->Pump ( sliceMillisecs, nullptr ) ) )
                return false;
        return true;
    }

    // Pump for at least `millisecs`, then return.  The caller-pumped stand-in
    // for a Sleep: on one of these hubs a sleep is exactly the wrong thing,
    // since nothing at all happens while the owning thread is not pumping.
    void pumpFor ( unsigned int millisecs, unsigned int sliceMillisecs = 10 )
    {
        ULONGLONG until = ::GetTickCount64() + millisecs;
        while ( ::GetTickCount64() < until )
            if ( FAILED ( m_pHub->Pump ( sliceMillisecs, nullptr ) ) )
                return;
    }

    // Queued and not yet dispatched.  A hint that lets an idle poll skip the
    // call; 0 does not prove Pump would find nothing.  Any hub, any thread.
    unsigned int pending ( ) const
    {
        unsigned int n = 0;
        if ( m_pHub ) m_pHub->GetPending ( &n );
        return n;
    }

    // Whose thread this hub runs on.
    bool callerPumped ( ) const
    {
        unsigned int f = 0;
        return m_pHub && SUCCEEDED ( m_pHub->GetPumpInfo ( &f, nullptr ) )
            && ( f & P2PF_PUMP_CALLER_DRIVEN ) != 0;
    }
    // TRUE when the CALLING thread is this hub's pump -- i.e. exactly when a
    // ping()/disconnect() on a SPAWNED hub would answer P2PF_E_PUMP_THREAD.
    bool onPumpThread ( ) const
    {
        unsigned int f = 0;
        return m_pHub && SUCCEEDED ( m_pHub->GetPumpInfo ( &f, nullptr ) )
            && ( f & P2PF_PUMP_THIS_THREAD ) != 0;
    }

    // The escape hatch: the P2PeerHub behind this hub. Cast it and you need
    // Targetcore's headers, its lib, MFC and its threading rules -- see
    // IP2PHub::GetNative. nullptr when the hub is closed.
    void* native ( ) const
    {
        void *p = nullptr;
        if ( m_pHub ) m_pHub->GetNative ( &p );
        return p;
    }

    void  close ( )
    {
        if ( m_pHub ) { m_pHub->Close(); m_pHub = nullptr; }
        m_sink.reset();
    }

    IP2PHub* raw ( ) const            { return m_pHub; }
    // The sink this hub registered, for the rare call site that needs to hand
    // it to the flat ABI itself (re-registering after SetExtEvents(nullptr),
    // which is really only a thing a test does).
    IP2PHubEvents2* rawSink ( ) const { return m_sink.get(); }
    explicit operator bool ( ) const  { return m_pHub != nullptr; }

  private:
    friend class Network;

    // The flat sink the facade calls on the pump thread; dispatches into the
    // std::function registries.  Heap-held so its address is stable.
    //
    // It implements the EXTENDED interface and is registered as both, so this
    // layer gets the ABI 6 callbacks -- and because the facade then delivers
    // OnMessageEx/OnEvent INSTEAD of OnMessage/OnError (the substitution rule
    // on IP2PHubEvents2), the two extended handlers below fall back to the
    // plain registries.  A client written against ABI 5 sees no difference.
    struct FnSink : public IP2PHubEvents2
    {
        // One delivery, decorated with what ABI 7 added.
        //
        // GetMsgInfo is asked HERE, which is the only place with the standing
        // to ask: it is defined on the pump thread inside the callback, and
        // this is that callback.  The scalars are free -- one call, no buffer.
        // The destination is not, so it is left to Message::destination(),
        // which asks while the same delivery is still running.
        Message make ( const wchar_t *source, const wchar_t *topic
                     , const void *payload, unsigned int size, bool broadcast )
        {
            Message m;
            m.source    = source;
            m.topic     = topic;
            m.payload   = payload;
            m.size      = size;
            m.broadcast = broadcast;
            m.hub       = m_pHub;
            if ( m_pHub )
                m_pHub->GetMsgInfo ( nullptr, nullptr, &m.priority, &m.tag,
                                     &m.flags );
            return m;
        }

        virtual void OnMessage ( const wchar_t *source, const wchar_t *topic
                               , const void *payload, unsigned int size
                               , bool broadcast )
        {
            Message m = make ( source, topic, payload, size, broadcast );
            auto it = m_topics.find ( topic );
            if ( it != m_topics.end() && it->second ) { it->second ( m ); return; }
            if ( m_fallback )                           m_fallback ( m );
        }
        virtual void OnPeerUp   ( const wchar_t *peer ) { if ( m_peerUp )   m_peerUp   ( peer ); }
        virtual void OnPeerDown ( const wchar_t *peer ) { if ( m_peerDown ) m_peerDown ( peer ); }
        virtual void OnError    ( const wchar_t *what ) { if ( m_error )    m_error    ( what ); }

        virtual HRESULT OnMessageEx ( const wchar_t *source, const wchar_t *topic
                                    , const void *payload, unsigned int size
                                    , bool broadcast )
        {
            if ( m_filter )
            {
                Message m = make ( source, topic, payload, size, broadcast );
                return m_filter ( m );
            }
            OnMessage ( source, topic, payload, size, broadcast );
            return S_OK;
        }
        virtual void OnEvent ( unsigned int code, const wchar_t *peer
                             , const wchar_t *what )
        {
            if ( m_event ) { m_event ( code, peer, what ); return; }
            if ( m_error )   m_error ( what );     // ABI 5 shape, unchanged
        }
        virtual void OnTimer ( unsigned int timerId, unsigned int key )
                                                    { if ( m_timer ) m_timer ( timerId, key ); }
        virtual void OnPost  ( unsigned int key, void *context )
                                                    { if ( m_post )  m_post  ( key, context ); }

        // The hub this sink belongs to, so make() can ask it about the message
        // it is delivering.  Filled by Network::createHub the moment CreateHub
        // hands the pointer back -- before any message can arrive, because the
        // hub has no connections yet.
        IP2PHub       *m_pHub = nullptr;

        std::map<std::wstring, MessageHandler> m_topics;
        MessageHandler m_fallback;
        PeerHandler    m_peerUp, m_peerDown;
        ErrorHandler   m_error;
        MessageFilter  m_filter;
        EventHandler   m_event;
        TimerHandler   m_timer;
        PostHandler    m_post;
    };

    void swap ( Hub& rhs )
    {
        std::swap ( m_pHub, rhs.m_pHub );
        std::swap ( m_sink, rhs.m_sink );
    }

    IP2PHub                *m_pHub = nullptr;
    std::unique_ptr<FnSink> m_sink;
};

inline Hub Network::createHub ( const wchar_t *address, unsigned int flags )
{
    Hub hub;
    hub.m_sink = std::make_unique<Hub::FnSink>();
    HRESULT hr = m_pNet->CreateHubEx ( address, hub.m_sink.get(), flags
                                     , &hub.m_pHub );
    if ( FAILED(hr) )
        throw std::runtime_error ( "IP2PNetwork::CreateHubEx failed" );
    hub.m_sink->m_pHub = hub.m_pHub;      // before anything can be delivered
    // One object, both interfaces: CreateHub took the plain half, this hands
    // over the extended one.  Done here rather than left to the caller so that
    // onTimer/onPost/onEvent work without a second call nobody would remember.
    hub.m_pHub->SetExtEvents ( hub.m_sink.get() );
    return hub;
}

} // namespace p2pf
