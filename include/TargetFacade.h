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
// TargetFacade.h
//
// The ONE public header of TargetFacade.dll -- a minimal, macro-free facade
// over TargetCore.dll.
//
// Design rules (all deliberate, do not "improve" them away):
//
//  * NOTHING from TargetCore leaks through here: no P2PeerHub, no P2PeerCon,
//    no P2PeerMsg, no MFC, no BEGIN_*_MAP macros.  A client includes only
//    this header and links only TargetFacade.lib.
//
//  * Pure-vtable interfaces + one extern "C" factory.  No C++ classes are
//    exported, so the facade is usable from any MSVC toolset (and any
//    language that can call a vtable) without name-mangling / CRT coupling.
//
//  * Every method returns HRESULT and takes only flat, automation-friendly
//    parameters (const wchar_t*, integers, void*+size).  That is intentional:
//    if/when an ATL layer with DUAL interfaces (IDispatch + vtable) is built
//    on top, each method here maps 1:1 onto a [dual] interface method
//    (LPCWSTR->BSTR, void*+size->SAFEARRAY(VT_UI1)), and IP2PHubEvents maps
//    onto a connection-point event interface.  See README.md "ATL road map".
//
//  * No STL types and no exceptions cross the DLL boundary.  A header-only
//    std::function convenience layer lives in TargetFacadeFn.hpp.
//
// Lifecycle in one sentence:
//    P2PF_CreateNetwork() -> IP2PNetwork::CreateHub() -> hub->Listen/Connect
//    -> hub->Send/Broadcast + IP2PHubEvents callbacks -> network->Release().
//
// Threading:
//  * All IP2PHub / IP2PNetwork methods may be called from any thread.
//  * IP2PHubEvents callbacks arrive on the hub's internal pump thread(s),
//    NEVER on the caller's thread.  Do not block in a callback; do not call
//    back into the same hub's Close() from a callback.
//  * Payload pointers passed to callbacks are valid ONLY for the duration of
//    the callback -- copy the bytes if you keep them.

#pragma once

#ifndef _WINDEF_
  #include <windows.h>   // HRESULT, wchar_t plumbing; clients all target Win32
#endif

#if defined(TARGETFACADE_EXPORTS)
  #define P2PF_API __declspec(dllexport)
#else
  #define P2PF_API __declspec(dllimport)
#endif

namespace p2pf {

// ---------------------------------------------------------------------------
// ABI version.
//
// ABI 2 and 3 were APPEND-ONLY: 2 added ListenEx/ConnectEx to IP2PHub and Link
// to IP2PNetwork, 3 added the read side, all after the methods that were
// already there -- so a v1 client saw an unchanged vtable PREFIX and needed no
// rebuild.
//
// ABI 4 IS THE HARD CUT, and it is the one kind of change that mechanic cannot
// absorb.  The eight typed arming verbs (Listen/Connect/ListenPipe/ConnectPipe/
// ListenDmx/ConnectDmx/ListenSerial/ConnectSerial) are GONE from the vtable,
// and ListenEx/ConnectEx have taken their names and their slots -- because the
// `Ex` suffix only ever existed to avoid colliding with the eight, and with
// them removed there is nothing to avoid.  Every slot after the arming pair has
// therefore moved.  A binary built against an older header would call the wrong
// method with the wrong arguments, so a v1-v3 client gets P2PF_E_ABI_MISMATCH
// instead of a vtable skew.  Recompile against this header; there is no
// compatible prefix to fall back on.
//
// ABI 5 IS APPEND-ONLY AGAIN, and deliberately so: it adds SetEndpoint /
// SetEndpointMap / GetEndpointFor to the END of IP2PNetwork and touches nothing
// else -- IP2PHub is untouched, every existing slot keeps its index.  So the
// factory accepts 4 AND 5: a binary built against the ABI 4 header runs on this
// DLL unchanged, it simply cannot see the three new methods.  The hard cut was
// a one-off, not a new habit.
//
// ABI 6 IS APPEND-ONLY TOO, and it is the first one to append to IP2PHub: ten
// methods after Describe (see "Reaching past the messaging slice" below), plus
// an OPTIONAL second sink interface, IP2PHubEvents2.  Nothing above them moves
// and IP2PHubEvents itself is untouched -- which is the whole reason the new
// callbacks live on a second interface a client OPTS INTO with SetExtEvents
// rather than as four more pure virtuals on the sink every existing client
// already implements.  Adding to a client-implemented interface is not
// append-only in any useful sense: the DLL would call a vtable slot the client
// does not have.  So the factory accepts 4, 5 AND 6.
//
// ABI 7 APPENDS THREE MORE TO IP2PHub and NOTHING ELSE -- SendEx, BroadcastEx
// and GetMsgInfo, after GetNative (see "The message model" below).  Note where
// they are NOT: there is no IP2PHubEvents3.  The receive half of this ABI is a
// method on IP2PHub -- which the FACADE implements -- rather than a callback on
// the sink, which the CLIENT implements.  ABI 6 had to mint a second sink
// interface for four new callbacks and make clients opt in; doing that again
// for every per-message field would end in IP2PHubEvents7, and would make a
// client rewrite its whole delivery handler to read one number.  Asking the hub
// about the message it is CURRENTLY delivering costs an existing ABI 6 client
// one line inside the callback it already has.  So the factory accepts 4, 5, 6
// AND 7.
//
// ABI 8 APPENDS ONE METHOD TO IP2PNetwork AND FIVE TO IP2PHub, and adds the
// first new interface since IP2PHubEvents2: IP2PMessage, a message with NAMED
// FIELDS beside its payload (see "Fields" below).  Note what it is not -- it is
// not a sink, and it is not a handle to anything the kernel owns.  It is a
// VALUE: a client fills one in, hands it to Send/Broadcast as an argument, and
// may hand the same one over again.  So it has no thread affinity, no hub
// reference, and no lifetime rule beyond Release.  Every existing slot keeps
// its index; the factory accepts 4 through 8.
//
// ABI 9 APPENDS ONE METHOD TO IP2PNetwork AND THREE TO IP2PHub, and changes no
// existing behaviour at all: CreateHubEx, and Pump / GetPending / GetPumpInfo
// (see "Who runs the pump" below).  It is the first ABI that lets a client
// decide WHICH THREAD the hub runs on.  Every hub this facade has ever created
// owns a thread it spawned; a CALLER-PUMPED hub runs on the thread that created
// it and does nothing until that thread calls Pump.  A client that never calls
// CreateHubEx gets a spawned hub and cannot tell this ABI from ABI 8.  So the
// factory accepts 4 through 9.
//
// ABI 10 APPENDS EIGHT METHODS TO IP2PNetwork, NONE TO IP2PHub, and adds one
// new client-implemented interface, IP2PDiagEvents: THE KERNEL'S OWN DIAGNOSTIC
// STREAM (see "Diagnostics" below).  Until now a client got one prose sentence
// through OnError and a code through OnEvent, both of them about ONE HUB and
// both of them raised by the facade; underneath, TargetCore has been narrating
// everything it does -- errors, warnings, traces, its own log lines -- to a
// callback slot no facade client could reach.  SetDiagSink takes that slot with
// a severity mask, RaiseDiag writes into it, and the two together are what a log
// view, a log filter or a severity gate is built from.  A client that never
// calls SetDiagSink registers nothing and pays nothing.  It is on the NETWORK
// and not on a hub because the stream is the PROCESS'S -- read "Diagnostics"
// before using it, because the delivery thread rule is not the one every other
// callback in this ABI follows, and it could not be.  The factory accepts
// 4 through 10.
//
// ABI 11 APPENDS ONE METHOD TO IP2PNetwork AND ONE TO IP2PHub, adds no new
// interface at all, and adds one bit to the flags word CreateHubEx already
// takes: P2PF_HUB_SECURE -- A SECURE HUB (see "Security" below).  Underneath,
// TargetCore has carried a signed login, a per-connection session cypher, an
// allow-list, a revocation list and an arming gate for some time, and none of
// it was reachable from here: the facade has been spawning every hub with the
// enforcement switches turned OFF, which is the library's own documented
// migration for a tree that has not been provisioned and is what this ABI keeps
// as the DEFAULT for a hub created without the flag.
//
// THE FLAG IS ON THE HUB BECAUSE THE SETTING IS ON THE HUB.  Enforcement in
// TargetCore is hub-wide and there is no per-connection override, so "is this
// link authenticated" was never a question one link could answer: a hub either
// demands a signed login from everything that reaches it or from nothing.
// Saying it once, when the hub is made and before it can have a connection at
// all, is therefore the only place the answer is not retroactive -- and it is
// what makes the rest of it invisible.  A secure hub generates its own keys,
// publishes their halves, and puts each new peer into its allow-list as the
// link is armed; no client of this facade names a key file, an allow-list or a
// revocation list, and none of them appears in this header.
//
// SetSecurityDir says where that material lives and has a default that works.
// IP2PHub::GetSecurityInfo reads a posture back.  Both are optional: a client
// that only passes the flag never calls either, and a client that never passes
// the flag cannot tell this ABI from ABI 10.  The factory accepts 4 through 11.
// ---------------------------------------------------------------------------
const unsigned int ABI_VERSION = 11;

// The oldest ABI whose vtable is still a prefix of this one.  Between this and
// ABI_VERSION inclusive, a client needs no rebuild.
const unsigned int ABI_VERSION_MIN = 4;

// ---------------------------------------------------------------------------
// Facade-specific HRESULTs (FACILITY_ITF, codes 0x0200+ as COM prescribes)
//
// These values are compared NUMERICALLY by com\test\ComSmokeTest.cpp and by
// the script clients, so codes are only ever appended -- never renumbered.
// ---------------------------------------------------------------------------
const HRESULT P2PF_E_ABI_MISMATCH   = MAKE_HRESULT(1, FACILITY_ITF, 0x0200); // header/DLL ABI_VERSION differ
const HRESULT P2PF_E_STARTUP        = MAKE_HRESULT(1, FACILITY_ITF, 0x0201); // StartupP2Pmsg/WSAStartup failed
const HRESULT P2PF_E_HUB_SPAWN      = MAKE_HRESULT(1, FACILITY_ITF, 0x0202); // hub pump thread failed to start
const HRESULT P2PF_E_CON_FACTORY    = MAKE_HRESULT(1, FACILITY_ITF, 0x0203); // transport endpoint creation failed
const HRESULT P2PF_E_CON_DUPLICATE  = MAKE_HRESULT(1, FACILITY_ITF, 0x0204); // peer address already posted on this hub
const HRESULT P2PF_E_RESERVED_TOPIC = MAKE_HRESULT(1, FACILITY_ITF, 0x0205); // topic starts with "P2Pmsg" (kernel-reserved)
const HRESULT P2PF_E_CLOSED         = MAKE_HRESULT(1, FACILITY_ITF, 0x0206); // hub/network already closed
const HRESULT P2PF_E_HUB_DUPLICATE  = MAKE_HRESULT(1, FACILITY_ITF, 0x0207); // a live hub already answers to this address
// --- added with the endpoint grammar ---------------------------------------
const HRESULT P2PF_E_ENDPOINT       = MAKE_HRESULT(1, FACILITY_ITF, 0x0208); // endpoint string unparseable or unsupported
const HRESULT P2PF_E_UNRESOLVED     = MAKE_HRESULT(1, FACILITY_ITF, 0x0209); // endpoint omitted and nothing to resolve it to
const HRESULT P2PF_E_NO_HUB         = MAKE_HRESULT(1, FACILITY_ITF, 0x020A); // Link: no live hub of that address in this process
const HRESULT P2PF_E_LINK_PARTIAL   = MAKE_HRESULT(1, FACILITY_ITF, 0x020B); // Link failed AND the listener could not be retracted
// SUCCESS code: the connection WAS armed, but this hub's address and `peer`
// are neither ancestor nor descendant of one another.  Direct traffic between
// the two works; the edge can never be a TRANSIT hop.  Test with
// `hr == P2PF_S_UNRELATED_LINK`, not with FAILED() -- see "Topology" on
// IP2PHub below.
const HRESULT P2PF_S_UNRELATED_LINK = MAKE_HRESULT(0, FACILITY_ITF, 0x020C);
// --- added with ABI 6 -------------------------------------------------------
const HRESULT P2PF_E_NO_PEER        = MAKE_HRESULT(1, FACILITY_ITF, 0x020D); // this hub has no connection for that peer
const HRESULT P2PF_E_TIMEOUT        = MAKE_HRESULT(1, FACILITY_ITF, 0x020E); // Ping: no answer inside the budget
const HRESULT P2PF_E_NO_SINK        = MAKE_HRESULT(1, FACILITY_ITF, 0x020F); // needs SetExtEvents; nothing would receive it
const HRESULT P2PF_E_OPTION         = MAKE_HRESULT(1, FACILITY_ITF, 0x0210); // unknown option, or set on a read-only one
const HRESULT P2PF_E_PUMP_THREAD    = MAKE_HRESULT(1, FACILITY_ITF, 0x0211); // a waiting call was made FROM the pump thread
// --- added with ABI 7 -------------------------------------------------------
const HRESULT P2PF_E_NO_MESSAGE     = MAKE_HRESULT(1, FACILITY_ITF, 0x0212); // GetMsgInfo outside a delivery callback
// --- added with ABI 8 -------------------------------------------------------
const HRESULT P2PF_E_NO_FIELD       = MAKE_HRESULT(1, FACILITY_ITF, 0x0213); // this message carries no such field
const HRESULT P2PF_E_FIELD_LIMIT    = MAKE_HRESULT(1, FACILITY_ITF, 0x0214); // too many fields, or a name too long
// --- added with ABI 9 -------------------------------------------------------
const HRESULT P2PF_E_NOT_PUMPED     = MAKE_HRESULT(1, FACILITY_ITF, 0x0215); // Pump on a hub that runs its own thread
const HRESULT P2PF_E_PUMP_OWNER     = MAKE_HRESULT(1, FACILITY_ITF, 0x0216); // a caller-pumped hub was touched from the wrong thread
// --- added with ABI 10 ------------------------------------------------------
const HRESULT P2PF_E_NO_DIAG        = MAKE_HRESULT(1, FACILITY_ITF, 0x0217); // GetDiagText/GetDiagInfo outside an OnDiag callback
// --- added with ABI 11 ------------------------------------------------------
const HRESULT P2PF_E_SECURITY       = MAKE_HRESULT(1, FACILITY_ITF, 0x0218); // a secure hub could not be provisioned, would not arm, or has no key for this peer

// ---------------------------------------------------------------------------
// IP2PHub::GetCon flags
// ---------------------------------------------------------------------------
// What this hub DID.  Mutually exclusive, and NEITHER bit is meaningful: it
// says the hub never armed anything for this peer -- the name was adopted by a
// wildcard listener when the far side logged in, or the peer arrived on a
// connection spawned from one.  Such a peer has no endpoint of ours to report.
const unsigned int P2PF_CON_LISTEN     = 0x0001;   // armed a passive endpoint
const unsigned int P2PF_CON_DIAL       = 0x0002;   // armed an active endpoint
// Live state, as of the call.
const unsigned int P2PF_CON_UP         = 0x0004;   // logged in right now

// Where the peer sits in the dotted address tree relative to this hub.
// EXACTLY ONE of these is set.  This is the half of the read side worth having:
// the kernel never checks that a connection joins an ancestor to a descendant,
// and a wrongly-shaped link connects, logs in and looks healthy -- so "is this
// peer my child, my parent, or neither" turns a silent class of topology bug
// into a one-call diagnosis.  See "Topology" on IP2PHub.
const unsigned int P2PF_REL_SELF       = 0x0100;   // same address as this hub
const unsigned int P2PF_REL_DESCENDANT = 0x0200;   // below me: "App" -> "App.B"
const unsigned int P2PF_REL_ANCESTOR   = 0x0400;   // above me: "App.B" -> "App"
const unsigned int P2PF_REL_UNRELATED  = 0x0800;   // sibling, or another tree
const unsigned int P2PF_REL_PATTERN    = 0x1000;   // a P2Padomain pattern: unknowable
const unsigned int P2PF_REL_MASK       = 0x1F00;

// ---------------------------------------------------------------------------
// IP2PHubEvents2::OnEvent codes  (ABI 6)
//
// The prose OnError string was the only push report the facade had, and the
// header has always told callers not to branch on its wording.  These are what
// to branch on instead; the text stays, as the human half of the same report.
// Codes are only ever appended.
// ---------------------------------------------------------------------------
const unsigned int P2PF_EVT_UNRELATED_LINK = 0x0001; // armed, but the peer is neither ancestor nor descendant
const unsigned int P2PF_EVT_DIAL_GAVE_UP   = 0x0002; // a derived dmx:// dial spent its rendezvous budget
// A message this hub SENT came back: no route to the destination, or a peer
// that would not handle it (including one whose OnMessageEx declined it).
// `peer` is the hub that bounced it.
const unsigned int P2PF_EVT_ROUTING_ERROR  = 0x0003;
const unsigned int P2PF_EVT_HUB_ERROR      = 0x0004; // anything else the hub wants to say

// ---------------------------------------------------------------------------
// IP2PHub::SetConOption / GetConOption  (ABI 6)
//
// ONE pair of methods rather than one pair per knob, for the reason the arming
// verbs are one pair rather than eight: a new knob is then a case label, not
// two more vtable slots in every language binding.  Values are all unsigned
// int, which is what survives every automation tier unchanged.
// ---------------------------------------------------------------------------
const unsigned int P2PF_OPT_TRACE     = 1;  // get/set, 0|1 -- P2Peerio wire trace
const unsigned int P2PF_OPT_MAXSEND   = 2;  // get/set, bytes (kernel default 32768)
const unsigned int P2PF_OPT_MAXRECV   = 3;  // get/set, bytes
const unsigned int P2PF_OPT_ENCRYPTED = 4;  // GET ONLY, 0|1 -- key exchange completed
const unsigned int P2PF_OPT_CONSTATE  = 5;  // GET ONLY, the P2PF_CONSTATE_* bits below
const unsigned int P2PF_OPT_MODE      = 6;  // GET ONLY, P2PF_CONMODE_*

// The kernel's own ConState_* bits, repeated here so a client never needs
// P2PeerCon.h.  Values match the kernel and are part of this ABI.
const unsigned int P2PF_CONSTATE_BCASTS      = 0x0001;
const unsigned int P2PF_CONSTATE_UCASTS      = 0x0002;
const unsigned int P2PF_CONSTATE_RECV        = 0x0004;
const unsigned int P2PF_CONSTATE_SEND        = 0x0008;
const unsigned int P2PF_CONSTATE_PKEY        = 0x0010;
const unsigned int P2PF_CONSTATE_LOGIN       = 0x0020;
const unsigned int P2PF_CONSTATE_CLOSEONIDLE = 0x0040;

const unsigned int P2PF_CONMODE_UNKNOWN = 0;
const unsigned int P2PF_CONMODE_DIAL    = 1;   // active   (kernel P2PeerCon_CLIENT)
const unsigned int P2PF_CONMODE_LISTEN  = 2;   // passive  (kernel P2PeerCon_SERVICE)
const unsigned int P2PF_CONMODE_ACCEPTED = 3;  // spawned by a listener on accept

// ---------------------------------------------------------------------------
// SendEx / BroadcastEx priorities  (ABI 7)
//
// The kernel's own P2PeerPri* values, repeated so a client never needs
// P2PeerMsg.h.  They are QUEUE positions inside one hub's pump, not a wire
// property and not a scheduling class: a lower number is served first when
// more than one message is waiting.  On a hub that is keeping up, nothing is
// ever waiting and priority changes nothing -- which is the honest description
// of what this buys and why the kernel's own header says to stick to Normal.
//
// P2PF_PRI_DEFAULT is NOT a kernel value.  It means "do not touch the
// priority", so SendEx with it posts a byte-identical message to Send, and is
// the value the plain Send/Broadcast use.
// ---------------------------------------------------------------------------
const unsigned int P2PF_PRI_FLUSH   = 0;   // reserved by the kernel for control
const unsigned int P2PF_PRI_HIGH    = 2;
const unsigned int P2PF_PRI_NORMAL  = 7;   // the kernel's default
const unsigned int P2PF_PRI_LOW     = 12;
const unsigned int P2PF_PRI_DEFAULT = 0xFFFFFFFF;  // leave the message alone

// ---------------------------------------------------------------------------
// SendEx / BroadcastEx flags  (ABI 7)
// ---------------------------------------------------------------------------
// Fire and forget: a peer that will not handle this message says nothing back.
//
// This exists because of what §5.4 of missing_progress.md changed: a message
// nobody handles is now REPORTED to its sender, as OnEvent with
// P2PF_EVT_ROUTING_ERROR.  That is right for traffic a client cares about and
// wrong for the fire-and-forget kind -- telemetry, a heartbeat, a broadcast on
// a topic most peers do not subscribe to -- where every uninterested peer
// answers with a bounce and the sender's event handler becomes the busiest
// thing in the process.  Set this on that traffic.
//
// WHAT IT COVERS, exactly.  It travels with the message and is honoured by the
// RECEIVING hub, which then never generates the report at all -- so the saving
// is the whole round trip, not just a suppressed callback.  It covers the
// message a peer DECLINES (an OnMessageEx that answered S_FALSE) and the one
// no handler claims.
//
// WHAT IT DOES NOT COVER: an address that could not be routed to.  That bounce
// is posted by the kernel's own RouteP2PeerMsg before any facade code runs,
// and nothing above the kernel can stop it.  A wrong address still reports;
// an uninterested peer does not.  The two are different mistakes and only one
// of them is noise.
//
// (Both halves are the facade's own doing.  P2PeerMsg has a control byte for
// precisely this -- P2PeerMsgCtrl_EXCEPTIONS -- and P2PeerMsg::Exceptions(),
// the toggle four kernel comments point at, is declared and implemented
// nowhere: linking against it fails, and no code anywhere consults that bit.
// See FacadeHub.cpp, kCtrlNoBounce.)
const unsigned int P2PF_SEND_NO_BOUNCE = 0x0001;

// ---------------------------------------------------------------------------
// IP2PHub::GetMsgInfo flags  (ABI 7)
// ---------------------------------------------------------------------------
const unsigned int P2PF_MSG_BROADCAST = 0x0001; // arrived as a broadcast
// Declining this message will actually reach its sender -- i.e. it was NOT
// sent with P2PF_SEND_NO_BOUNCE.  Set on every message from a peer that is not
// this facade, and on every message any ABI 5 or 6 client ever sent, because
// the suppression is what is recorded and its absence is the default.
const unsigned int P2PF_MSG_BOUNCES   = 0x0002;
const unsigned int P2PF_MSG_WRAPPED   = 0x0004; // carries a wrapped message
const unsigned int P2PF_MSG_REFLECTED = 0x0008; // came back off a reflector
const unsigned int P2PF_MSG_FIELDS    = 0x0010; // carries named fields (ABI 8)

// ---------------------------------------------------------------------------
// Named fields  (ABI 8)
//
// A message's payload is one opaque run of bytes, which means a client sending
// anything with STRUCTURE has to invent an encoding for it and agree that
// encoding at both ends -- the one thing this facade otherwise never asks of
// anybody.  Fields are the alternative: a flat `name -> bytes` map carried
// BESIDE the payload, not inside it.
//
// Beside, not inside, is the whole compatibility argument.  The bytes given to
// Send stay exactly where they have always been (the kernel's message-data
// item), so a peer that is not this facade, or is an older one, reads the same
// payload it always read and never learns the fields exist.  Adding fields to
// a message can therefore never break a receiver.
//
// A flat map, not the kernel's full P3PmsgItem tree.  The tree is arbitrarily
// deep and the kernel uses that depth for its own envelopes; applications put
// records in messages, and a record is a flat map.  Projecting the whole tree
// would need a node type in this header, which the founding rules forbid, and
// would buy a shape almost nobody sends.
// ---------------------------------------------------------------------------
const unsigned int MAX_FIELDS         = 64;    // per message
const unsigned int MAX_FIELD_NAME     = 63;    // characters, terminator extra
const unsigned int MAX_FIELD_SIZE     = 8192;  // bytes, per field

// Maximum payload per message: kernel MAX_P2Psize (32768) minus generous
// headroom for the message-tree overhead (addresses, name, node prefixes).
// An oversize frame makes the transport DROP the connection, so Send/Broadcast
// reject anything larger instead of putting it on the wire.
const unsigned int MAX_PAYLOAD = 24 * 1024;

// ---------------------------------------------------------------------------
// Who runs the pump  (ABI 9)
//
// Every hub has exactly one pump, and the pump is a THREAD: the kernel keys a
// pump by the id of the thread it was constructed on, and refuses to give one
// thread two (P2Pwin32.cpp, the P2PmsgPump constructor and CreateP2PmsgPump's
// opening guard).  The only question this ABI has ever answered for a client
// is "whose thread?", and until now it answered it one way -- the facade
// spawned one and never said which it was.
//
// CreateHubEx with P2PF_HUB_CALLER_PUMPED answers it the other way.  The hub is
// created on the CALLING thread, and from then on nothing whatever happens on
// it -- no connect, no login, no delivery, no timer, no close -- until that
// same thread calls Pump.  That is the point: a GUI or service host that
// already owns a loop gets its callbacks ON that loop, with its own state, and
// needs no lock, no marshalling and no Post round trip to touch anything the
// callbacks touch.
//
// WHAT IT COSTS.  A caller-pumped hub is only as live as its loop.  Stop
// calling Pump and the hub stops answering -- logins stall, timers do not fire,
// peers eventually see the connection go idle.  A spawned hub cannot be starved
// this way, which is why it stays the default and why CreateHub is untouched.
//
// THREAD AFFINITY IS ABSOLUTE.  Pump, and every call that WAITS for the pump
// (Disconnect, Ping, Close), must come from the creating thread; from any other
// they answer P2PF_E_PUMP_OWNER rather than doing something undefined.  Sends
// and the read side are unrestricted, exactly as they are on a spawned hub.
//
// THE ONE THING THIS BUYS BACK.  On a spawned hub, Disconnect and Ping refuse
// with P2PF_E_PUMP_THREAD when called from a callback, because the thread that
// would satisfy the wait is the one being blocked.  On a caller-pumped hub the
// caller's thread IS the pump, so instead of refusing they DRIVE the pump while
// they wait -- the deadlock the refusal existed to prevent cannot arise when
// the waiter is also the worker.  They are therefore usable from inside a
// callback here and nowhere else.
//
// Two consequences of that, both worth stating rather than discovering:
//
//   * A callback that calls Ping RE-ENTERS the pump, so a second message may be
//     delivered inside the first delivery.  GetMsgInfo and the field readers
//     then describe the INNER message for the length of that nested callback,
//     which is what they have always meant.
//   * ONE PLACE IT STILL WILL NOT WORK, and it is not a facade rule: a Ping to
//     the peer whose message you are CURRENTLY HANDLING times out.  Re-entering
//     the pump does not re-enter that connection, which cannot receive anything
//     further until the frame being dispatched is finished with -- so the
//     answer is unable to arrive before the budget expires.  Ping a different
//     peer, or ping from a timer or a Post callback, or answer later.  Measured
//     rather than reasoned; see section 26 of the smoke test.
// ---------------------------------------------------------------------------
// CreateHubEx flags.
const unsigned int P2PF_HUB_SPAWN_PUMP    = 0x0000; // the default: what CreateHub does
const unsigned int P2PF_HUB_CALLER_PUMPED = 0x0001; // this thread owns the pump
// A SECURE HUB: it holds an identity, it demands a signed login from every peer
// it links to, and it will not carry a peer it cannot authenticate.  (ABI 11)
// Orthogonal to the pump flag -- either kind of hub can be secure.  See
// "Security" further down, and IP2PHub::GetSecurityInfo.
const unsigned int P2PF_HUB_SECURE        = 0x0002; // authenticated, per hub

// What one Pump call actually did, reported through its `outWhat`.  These are
// the kernel's own P2PmsgPump_* result codes, repeated so a client never needs
// P2Pwin32.h.  They are diagnostic -- a loop wants the HRESULT, not this.
const unsigned int P2PF_PUMP_NOTHING = 1;   // the budget expired with no work
const unsigned int P2PF_PUMP_MESSAGE = 2;   // a message was dispatched
const unsigned int P2PF_PUMP_CON     = 3;   // connection activity
const unsigned int P2PF_PUMP_IO      = 4;   // a completed transport operation
const unsigned int P2PF_PUMP_SIGNAL  = 5;   // a hub/pump signal
const unsigned int P2PF_PUMP_TIMER   = 6;   // a timer fired

// GetPumpInfo flags.
const unsigned int P2PF_PUMP_CALLER_DRIVEN = 0x0001; // created with P2PF_HUB_CALLER_PUMPED
const unsigned int P2PF_PUMP_THIS_THREAD   = 0x0002; // the caller owns it

// ---------------------------------------------------------------------------
// Diagnostics                                                        (ABI 10)
//
// TargetCore narrates itself.  Every refused login, dropped connection,
// unroutable message, oversize frame and internal assumption it checks raises a
// P2Pevent -- an object with a SEVERITY, a serial number, the function that
// raised it, a sentence, often an advice line and sometimes an HRESULT.  Until
// this ABI none of it reached a facade client: OnError got one sentence the
// header tells you not to parse, and OnEvent got a code for the four conditions
// the FACADE raises.  Everything the kernel itself had to say went nowhere.
//
// SetDiagSink is where it goes now.  FIVE things about it are properties of the
// kernel rather than choices, and every one of them is visible from out here:
//
//   1. THE STREAM IS THE PROCESS'S.  A P2Pevent carries no hub address, and the
//      thing that dispatches it is one static slot in Msgcore -- not a per-hub
//      registration.  So this is on IP2PNetwork, there is exactly one sink, and
//      it hears every hub, every connection, and kernel code running on threads
//      belonging to none of them.
//   2. THERE IS ONLY ONE SLOT IN THE PROCESS, and taking it is not free.  The
//      kernel's own file logger (MsgexceptionLog) registers in the same slot,
//      last writer wins, and nothing anywhere can read the previous occupant
//      back to chain to it.  So SetDiagSink DISPLACES that logger if it is
//      running, and starting it afterwards displaces this.  There is no way to
//      have both, and pretending otherwise would be worse than saying it.
//   3. DELIVERY IS SYNCHRONOUS, ON THE THREAD THAT RAISED THE EVENT.  This is
//      the one callback in this ABI that does NOT arrive on a pump thread, and
//      it cannot: the kernel calls the slot inline as the event is disposed of,
//      and no hub is involved to marshal it to.  Consequences, all of them
//      yours to handle:
//         * OnDiag may run on SEVERAL THREADS AT ONCE.  Make it thread-safe.
//         * It may run on YOUR OWN thread, inside a facade call that failed.
//         * A slow handler slows down whatever the kernel was doing.  Format
//           and queue; do not write to a network share.
//      GetDiagInfo reports which thread it was, which is the column that makes
//      a kernel log readable in the first place.
//   4. THE MASK IS THE FACADE'S FILTER, NOT THE KERNEL'S.  The slot is handed
//      every event regardless; the facade tests the class and returns.  So a
//      narrow mask saves your handler, not the kernel -- ask for what you will
//      read anyway.  P2PF_DIAGM_ALL on a busy process is a firehose, and in a
//      Debug build the kernel is chatty.
//   5. IT IS BEST-EFFORT BY CONSTRUCTION.  An event reaches the slot when it is
//      CANCELLED, which is how the kernel disposes of them; one parked as the
//      thread's "last event" is notified only if something later cancels it,
//      and one displaced by the next is simply deleted.  Most of the stream
//      arrives.  Do not build anything that needs all of it -- eventNo is there
//      precisely so a gap can be seen rather than assumed away.
//
// The one rule this ABI adds, and it is the usual one for a logger: DO NOT
// RAISE FROM INSIDE THE HANDLER.  An OnDiag that causes another event feeds
// itself, and the facade cannot stop the kernel raising one -- what it can and
// does stop is the loop through its own front door: RaiseDiag called from
// inside an OnDiag delivery on that thread is suppressed and answers S_FALSE.
// ---------------------------------------------------------------------------
// Severity classes.  These are the kernel's own P2Pevent_e values, repeated
// here so a client never needs Msgexception.h; the facade maps them explicitly
// rather than casting, so they are this ABI's promise and not the kernel's.
const unsigned int P2PF_DIAG_ERROR    = 1;
const unsigned int P2PF_DIAG_WARNING  = 2;
const unsigned int P2PF_DIAG_INFO     = 3;
const unsigned int P2PF_DIAG_DEBUG    = 4;
const unsigned int P2PF_DIAG_TRACE    = 5;
const unsigned int P2PF_DIAG_LOG      = 6;
const unsigned int P2PF_DIAG_REPORT   = 7;
// The client's own class.  RaiseDiag accepts any of the above too, but this one
// is the kernel's reserved "not mine" class: nothing in TargetCore ever raises
// it, so a mask of P2PF_DIAGM_APP hears your application and nothing else.
const unsigned int P2PF_DIAG_APP      = 16;

// Masks, for SetDiagSink / SetDiagMask / IsDiagWanted: one bit per class.
const unsigned int P2PF_DIAGM_ERROR   = 1u << P2PF_DIAG_ERROR;
const unsigned int P2PF_DIAGM_WARNING = 1u << P2PF_DIAG_WARNING;
const unsigned int P2PF_DIAGM_INFO    = 1u << P2PF_DIAG_INFO;
const unsigned int P2PF_DIAGM_DEBUG   = 1u << P2PF_DIAG_DEBUG;
const unsigned int P2PF_DIAGM_TRACE   = 1u << P2PF_DIAG_TRACE;
const unsigned int P2PF_DIAGM_LOG     = 1u << P2PF_DIAG_LOG;
const unsigned int P2PF_DIAGM_REPORT  = 1u << P2PF_DIAG_REPORT;
const unsigned int P2PF_DIAGM_APP     = 1u << P2PF_DIAG_APP;
// What a service usually wants: the two classes that mean something is wrong.
const unsigned int P2PF_DIAGM_PROBLEMS = P2PF_DIAGM_ERROR | P2PF_DIAGM_WARNING;
// Everything.  The top bit is reserved by the kernel and is not settable.
const unsigned int P2PF_DIAGM_ALL      = 0x7FFFFFFFu;
// Nothing.  Passing this to SetDiagSink with a sink registers the sink and
// hears nothing, which is how a client arms a log view it will switch on later.
const unsigned int P2PF_DIAGM_NONE     = 0x00000000u;

// Which piece of text GetDiagText should hand back.  One selector rather than
// one method per part, for the reason SetConOption is one pair of methods
// rather than one pair per knob: a part added later costs a constant.
const unsigned int P2PF_DIAGT_MESSAGE = 1; // the sentence -- same as OnDiag's `text`
const unsigned int P2PF_DIAGT_MODULE  = 2; // the function that raised it
const unsigned int P2PF_DIAGT_ADVICE  = 3; // what the kernel suggests doing about it
const unsigned int P2PF_DIAGT_SERVICE = 4; // the process/service name, stamped at creation
const unsigned int P2PF_DIAGT_GROUP   = 5; // the kernel's own grouping tag ("P2P", ...)
// The severity's short name, as the KERNEL spells it ("EVERR", "EVTRC").  Its
// table stops at P2Pevent_INFO: every class from REPORT (7) upwards, including
// P2PF_DIAG_APP, reads back as the literal "EV005".  Reported as-is because it
// is what the kernel's own logs say; switch on `severity` for a label of your
// own rather than parsing this.
const unsigned int P2PF_DIAGT_CLASS   = 6;
const unsigned int P2PF_DIAGT_HRESULT = 7; // the HRESULT's text, when one was attached

// ---------------------------------------------------------------------------
// GetSecurityInfo flags -- what a hub's security posture actually is. (ABI 11)
//
// READ BACK FROM THE HUB, never from a copy the facade kept: every bit below is
// answered by asking the kernel at the moment of the call.  A cached posture is
// one forgotten line away from reporting a state the hub does not have, which
// for this particular question is the whole failure mode.
//
// The one that matters is P2PF_SEC_ARMED, and it is NOT implied by
// P2PF_SEC_REQUIRED: a hub can require authentication and be unable to perform
// it -- no identity, no allow-list, a revocation list that will not load -- and
// that combination is exactly what refuses every peer that arrives.  Nothing
// is ever armed on a secure hub until the hub answers ARMED, so seeing it here
// after a successful Link/Listen/Connect is a confirmation, not a hope.
//
// A SECURE HUB WITH NO PEERS YET reads REQUIRED=0 and ARMED=0, and that is the
// honest answer rather than a gap: enforcement is switched on when the hub
// gains the first peer it can authenticate, because the kernel refuses to
// START a hub that requires authentication and trusts nobody (p2pauth's
// ArmEmptyAllow -- an allow-list that lists nobody refuses everybody, so it is
// refused at startup instead of at the first login).  CAN_SIGN, CAN_OPEN and
// REVOCATION are all set from the moment the hub exists.
// ---------------------------------------------------------------------------
const unsigned int P2PF_SEC_NONE       = 0x0000; // a hub nothing has secured
const unsigned int P2PF_SEC_REQUIRED   = 0x0001; // enforcement is ON (auth is demanded)
const unsigned int P2PF_SEC_CAN_SIGN   = 0x0002; // holds an identity key: can prove itself
const unsigned int P2PF_SEC_CAN_OPEN   = 0x0004; // holds an agreement key: can be sealed to
const unsigned int P2PF_SEC_ARMED      = 0x0008; // can enforce what it requires, RIGHT NOW
const unsigned int P2PF_SEC_SEALED     = 0x0010; // a relayed body must be sealed end-to-end
const unsigned int P2PF_SEC_REVOCATION = 0x0020; // a revocation list is configured AND loads

// ---------------------------------------------------------------------------
// IP2PDiagEvents -- implemented by the CLIENT, handed to SetDiagSink. (ABI 10)
//
// A THIRD sink interface rather than a third generation of IP2PHubEvents, and
// deliberately not derived from either: there is no callback here that
// overlaps one of theirs, so there is no substitution rule to learn, and a
// client that wants its log window to receive this -- rather than the object
// that handles its messages -- simply hands over a different pointer.  ABI 6's
// rule about not appending to a client-implemented interface is why this is a
// new one; nothing about that rule says it has to be the same object.
//
// NOT delivered on a pump thread -- see "Diagnostics" above, point 3, which is
// the one paragraph of it that must be read before implementing this.  The sink
// is not owned; it must outlive the network or be taken back with
// SetDiagSink(NULL, 0).
// ---------------------------------------------------------------------------
struct IP2PDiagEvents
{
    // One event from the kernel's stream, filtered by the mask in force when
    // it was raised.
    //
    //   severity  one P2PF_DIAG_* value.  Branch on this.
    //   eventNo   a process-wide serial, counting EVERY event the facade has
    //             been handed since it took the slot -- including the ones
    //             your mask threw away.  So it makes a log line unique, and a
    //             GAP between two consecutive deliveries is the exact number
    //             of events your own filter cost you.  There is no other drop
    //             counter, and there is no kernel one either: P2Pevent carries
    //             a field documented as "internally allocated event number,
    //             auto-assigned in MakeEvent", and nothing in the kernel ever
    //             assigns it, so asking the event answers 0 forever.
    //   module    the function that raised it ("P2PeerCon::PostOVERLAPPED"),
    //             or an empty string.  Never NULL.
    //   text      the sentence.  Never NULL, occasionally empty, and may
    //             contain newlines: the kernel accumulates description lines
    //             and this is all of them joined.
    //
    // Everything else about the event -- its advice line, its group, its
    // HRESULT, the thread that raised it -- is read with
    // IP2PNetwork::GetDiagText and GetDiagInfo, and ONLY from inside this call.
    // Copy what you keep: the kernel destroys the event as this returns.
    virtual void OnDiag ( unsigned int   severity
                        , unsigned int   eventNo
                        , const wchar_t *module
                        , const wchar_t *text ) = 0;

  protected:
    ~IP2PDiagEvents ( ) { }
};

// ---------------------------------------------------------------------------
// IP2PHubEvents -- implemented by the CLIENT, one per hub.
//
// This is the macro-free replacement for deriving from P2PeerHub and writing
// BEGIN_P2PeerMsg_MAP / BEGIN_P2PeerCon_MAP entries: implement four plain
// virtuals (or subclass HubEventsBase below and pick the ones you care
// about; or use the std::function adapter in TargetFacadeFn.hpp).
//
// All callbacks fire on the hub's pump thread -- the one the facade spawned,
// or, for a hub created with P2PF_HUB_CALLER_PUMPED, the client's own thread
// from inside its call to Pump.  See "Who runs the pump" above.
// ---------------------------------------------------------------------------
struct IP2PHubEvents
{
    // A message addressed to this hub arrived (unicast, possibly multi-hop
    // routed) OR a broadcast reached it.  `broadcast` distinguishes the two.
    // `source` is the full address of the ORIGINATING hub, `topic` is the
    // application topic given to Send/Broadcast on that hub.
    virtual void OnMessage ( const wchar_t *source
                           , const wchar_t *topic
                           , const void    *payload
                           , unsigned int   size
                           , bool           broadcast ) = 0;

    // A connection to `peer` completed its login handshake (fires on the
    // listening side and on the dialing side alike).
    virtual void OnPeerUp   ( const wchar_t *peer ) = 0;

    // A connection to `peer` closed (remote gone, transport dropped, or the
    // hub is shutting down).  Auto-reconnecting dials re-fire OnPeerUp later.
    virtual void OnPeerDown ( const wchar_t *peer ) = 0;

    // Routing/kernel error surfaced for this hub (e.g. undeliverable
    // destination).  The hub keeps running.
    //
    // "Informational" understates it: some messages here are the only PUSHED
    // report of a condition the return value could not express.  The sibling-
    // link warning is one -- Listen/Connect return P2PF_S_UNRELATED_LINK to a
    // C++ caller, but every automation tier sees that success code normalised
    // to S_OK, and this event is what tells them.  Log it at warning level.
    // It is prose, though, not a code: branch on the return value here, or on
    // IP2PHub::GetCon's P2PF_REL_* flags (IP2PHubCom::RelationTo through COM),
    // never on the wording of this string.
    virtual void OnError    ( const wchar_t *what ) = 0;

  protected:
    ~IP2PHubEvents ( ) { }      // facade never deletes the client's sink
};

// Optional convenience: all-default sink, override only what you need.
// Header-only on purpose (never crosses the DLL boundary as a type).
struct HubEventsBase : public IP2PHubEvents
{
    virtual void OnMessage  ( const wchar_t*, const wchar_t*,
                              const void*, unsigned int, bool ) { }
    virtual void OnPeerUp   ( const wchar_t* ) { }
    virtual void OnPeerDown ( const wchar_t* ) { }
    virtual void OnError    ( const wchar_t* ) { }
};

// ---------------------------------------------------------------------------
// IP2PHubEvents2 -- the OPTIONAL extended sink (ABI 6).  Implemented by the
// client, handed over with IP2PHub::SetExtEvents, and never required: a hub
// with no extended sink behaves exactly as it did in ABI 5.
//
// Why a second interface instead of four more methods on IP2PHubEvents: that
// interface is implemented by the CLIENT.  Appending to it is not an
// append-only change at all -- the DLL would call through a vtable slot the
// client's object does not have, which is a crash rather than a mismatch the
// factory could catch.  A second interface the client opts into is the only
// shape that keeps every existing binary correct.
//
// THE SUBSTITUTION RULE, and it matters: once an extended sink is registered,
// the two callbacks that OVERLAP with the plain sink are delivered to the
// extended one INSTEAD, never to both.
//     OnMessage -> OnMessageEx
//     OnError   -> OnEvent
// Anything else (OnPeerUp/OnPeerDown) keeps going to the plain sink, which is
// why IP2PHubEvents2 derives from IP2PHubEvents: one object implements both
// halves and is handed to CreateHub and SetExtEvents in turn.
//
// All four fire on the hub's pump thread, under the same rules as the plain
// sink: do not block, do not call Close(), copy any payload you keep.
// ---------------------------------------------------------------------------
struct IP2PHubEvents2 : public IP2PHubEvents
{
    // Same delivery as OnMessage, plus an answer.  Return:
    //   S_OK    -- handled; routing stops here (what OnMessage always meant).
    //   S_FALSE -- NOT handled, and the SENDER is told: it receives OnEvent
    //              with P2PF_EVT_ROUTING_ERROR carrying the kernel's own
    //              "Message[topic] not handled" sentence.
    // The answer means the same thing on every path, but it is not reached the
    // same way on all of them.  A topic-named unicast simply travels on down
    // the kernel's handler chain, which ends in that report.  A broadcast
    // cannot: the P2PeerHub base handler is the relay that forwards it to child
    // hubs, so it must run whatever the client answers -- the facade therefore
    // sends the same report itself, after the relay.  Either way a decline
    // costs one message back to the sender, so decline what is not yours, not
    // what you simply have nothing to do about.
    virtual HRESULT OnMessageEx ( const wchar_t *source
                                , const wchar_t *topic
                                , const void    *payload
                                , unsigned int   size
                                , bool           broadcast ) = 0;

    // The structured half of OnError.  `code` is a P2PF_EVT_* value -- branch
    // on THAT.  `peer` is the connection the event is about, or an empty
    // string when it is about the hub.  `what` is the same sentence OnError
    // would have received.
    virtual void OnEvent   ( unsigned int   code
                           , const wchar_t *peer
                           , const wchar_t *what ) = 0;

    // A timer armed with IP2PHub::SetTimer has elapsed.  `key` is the value
    // passed to SetTimer; `timerId` is what SetTimer returned.  One shot --
    // re-arm from inside the callback if you want it periodic.
    virtual void OnTimer   ( unsigned int timerId, unsigned int key ) = 0;

    // Work handed to IP2PHub::Post from another thread, now running ON the
    // pump thread, in FIFO order with everything else that hub is doing.
    virtual void OnPost    ( unsigned int key, void *context ) = 0;

  protected:
    ~IP2PHubEvents2 ( ) { }
};

// All-default extended sink; override only what you need.
struct HubEventsBase2 : public IP2PHubEvents2
{
    virtual void    OnMessage   ( const wchar_t*, const wchar_t*,
                                  const void*, unsigned int, bool ) { }
    virtual void    OnPeerUp    ( const wchar_t* ) { }
    virtual void    OnPeerDown  ( const wchar_t* ) { }
    virtual void    OnError     ( const wchar_t* ) { }
    virtual HRESULT OnMessageEx ( const wchar_t*, const wchar_t*,
                                  const void*, unsigned int, bool ) { return S_OK; }
    virtual void    OnEvent     ( unsigned int, const wchar_t*, const wchar_t* ) { }
    virtual void    OnTimer     ( unsigned int, unsigned int ) { }
    virtual void    OnPost      ( unsigned int, void* ) { }
};

// ---------------------------------------------------------------------------
// IP2PMessage -- a payload plus named fields, ready to send        (ABI 8)
//
// THIS IS A VALUE, NOT A HANDLE.  It holds no hub, opens nothing, and talks to
// nothing; it is a bag of bytes with names on some of them.  That is what lets
// it have no rules:
//
//   * any thread may fill one in, including a pump-thread callback;
//   * it is not consumed by sending -- fill one in once and send it to twenty
//     peers, or edit one field and send it again;
//   * it outlives, or is outlived by, any hub without either caring;
//   * the only lifetime rule is that Release destroys it.
//
// Which is also why it is not created by a hub: it belongs to none.
// IP2PNetwork::CreateMessage makes one.  Sending is IP2PHub::SendMsg /
// BroadcastMsg -- the hub is the thing that can send, so the hub is where
// sending stays, and this object is the argument.
//
// Reading fields off a RECEIVED message is not done through this interface at
// all: see IP2PHub::GetField, which answers about the message being delivered
// under the same rule as GetMsgInfo.  Receiving is not a value, and pretending
// it was would mean copying every field of every message on the chance that a
// handler wanted one.
// ---------------------------------------------------------------------------
struct IP2PMessage
{
    // The opaque payload -- exactly what Send's `payload`/`size` mean, and it
    // travels in exactly the same place, so a receiver that knows nothing of
    // fields reads this and only this.  Optional: a message may be all fields
    // and no payload.
    virtual HRESULT SetPayload   ( const void *payload, unsigned int size ) = 0;

    // Set (or replace) one named field.  `name` is any client string of up to
    // MAX_FIELD_NAME characters that does not start with "P2PF" -- that prefix
    // is this facade's, for the same reason "P2Pmsg" is the kernel's.
    //
    // A NULL/zero-size value is legal and means an EMPTY field, which is a
    // different thing from an absent one: GetField answers S_OK with a size of
    // 0, where an absent field answers P2PF_E_NO_FIELD.  Presence is a signal
    // in its own right and this ABI keeps it.
    //
    // P2PF_E_FIELD_LIMIT past MAX_FIELDS fields, a name longer than
    // MAX_FIELD_NAME, or a value larger than MAX_FIELD_SIZE.
    virtual HRESULT SetField     ( const wchar_t *name
                                 , const void    *value
                                 , unsigned int   size ) = 0;

    // Text convenience: stores the UTF-16 string INCLUDING its terminator,
    // matching SendText, so the receiver can use the bytes as a string
    // directly.
    virtual HRESULT SetFieldText ( const wchar_t *name, const wchar_t *text ) = 0;

    // Remove one field.  S_FALSE if there was none of that name.
    virtual HRESULT RemoveField  ( const wchar_t *name ) = 0;

    // How many fields are set, and their names by position.  The ordering is
    // insertion order and is stable -- nothing else touches this object.
    // `buf`/`cch` follow the same buffer protocol as the rest of the ABI.
    virtual HRESULT GetFieldCount( unsigned int *outCount ) const = 0;
    virtual HRESULT GetFieldName ( unsigned int index
                                 , wchar_t *buf, unsigned int *cch ) const = 0;

    // Read one field back out of this object (the WRITE side's reader -- for
    // a received message use IP2PHub::GetField).  `size` in is the capacity of
    // `buf` in bytes, out is the size required, always; `buf` may be NULL to
    // ask the size alone.  ERROR_MORE_DATA if it does not fit, nothing
    // written.  P2PF_E_NO_FIELD if there is no such field.
    virtual HRESULT GetField     ( const wchar_t *name
                                 , void *buf, unsigned int *size ) const = 0;

    // Drop the payload and every field, so one object can be reused for a
    // message that has nothing to do with the last one.
    virtual HRESULT Clear        ( ) = 0;

    // Destroy it.  The pointer is invalid afterwards.
    virtual ULONG   Release      ( ) = 0;

  protected:
    ~IP2PMessage ( ) { }
};

// ---------------------------------------------------------------------------
// IP2PHub -- one messaging endpoint (wraps one P2PeerHub + its pump thread,
// both created and owned inside the facade).
//
// Listen/Connect arm one connection each and may be called repeatedly to give
// a hub many connections (any transport mix).  `toPeer` is the address of the
// hub on the OTHER end -- never this hub's own, which was fixed at CreateHub
// and is never passed again:
//   * on Connect it names who you are dialing;
//   * on Listen it names who you EXPECT to dial in -- one Listen per expected
//     peer.  Distinct peers need distinct addresses; the kernel rejects a
//     duplicate peer address on the same hub (P2PF_E_CON_DUPLICATE).
// It may also be a P2Padomain PATTERN ("*", "Demo.*", "A|B", "Node.#",
// "S.<A,B>"), which is what makes a listener serve many peers: the name the
// dialer claims at login becomes the connection's routing key.
//
// A hub stays useless until at least one Listen/Connect succeeds.
//
// Ordering does NOT matter for a tcp:// or pipe:// dial: they keep re-dialling
// (about twice a second) until the far side answers, and re-dial again if an
// established link later drops. So S_OK from Connect means "the dial is
// armed", not "the peer is up" -- wait for OnPeerUp (or poll IsPeerUp) for
// that. A peer that never appears simply never fires OnPeerUp; no error is
// raised, because "not up yet" is indistinguishable from "not up ever".
// A dmx:// or serial:// dial you SPELL OUT does NOT retry: a missing
// in-process service or COM port is a configuration fault, not a timing one,
// and it fails fast on purpose.
// A dial with an OMITTED endpoint does retry, up to 8 attempts, whatever
// transport it resolves to. Nothing was configured for it to be wrong about --
// the resolution already refused unless a live sibling hub in this process had
// recorded a listener expecting you -- so the only thing left to fail is that
// hub's listener arming a moment later on its own pump thread. If the attempts
// run out (the usual cause: that hub has since closed), the dial stops for good
// and raises ONE OnError saying so, which is the one dial failure the facade
// reports rather than leaving to OnPeerUp never arriving.
// ---------------------------------------------------------------------------
struct IP2PHub
{
    // --- connections -------------------------------------------------------
    //
    // ONE pair of verbs for every transport.  What used to be eight typed
    // methods was one method plus a four-way switch on how the endpoint is
    // spelled -- same null check, same factory call shape, same PostCon tail,
    // same HRESULTs.  So the endpoint is spelled ONE way, as a string, which
    // makes it a configurable VALUE (ini / registry / argv) instead of a
    // compile-time choice of method name, and makes a fifth transport a parser
    // entry rather than two more vtable slots.
    //
    //   scheme    listen form         connect form           retries?
    //   tcp       tcp://:7788         tcp://127.0.0.1:7788   yes
    //             (listen host must be empty, "*" or "0.0.0.0": the kernel
    //              binds INADDR_ANY unconditionally.  IPv4 only -- IPv6 is
    //              rejected, P2PeerConWsa is AF_INET.)
    //   pipe      pipe://name         pipe://name            yes
    //             (a bare name or a full \\.\pipe\name, taken verbatim)
    //   dmx       dmx://service       dmx://service          NO
    //   serial    serial://COM5       serial://5             NO
    //   (omitted) NULL / ""           NULL / ""              yes, 8 attempts
    //
    // `endpoint` may be NULL or empty, which asks the facade to resolve it:
    //   * Listen  -- arms an in-process Dmx service under a name derived
    //                from (this address, toPeer), so a sibling hub in the same
    //                process can find it with no configuration at all.
    //   * Connect -- looks for a live hub of address `toPeer` IN THIS PROCESS
    //                that has already armed a connection expecting THIS hub,
    //                and dials whatever endpoint that hub armed.  If there is
    //                no such hub, or it has not armed yet, the call fails
    //                with P2PF_E_UNRESOLVED -- it never guesses.  Use
    //                IP2PNetwork::Link to arm both sides in one ordered call.
    //                A resolved dial RETRIES (see above), so the far hub's
    //                listener may still be arming when this call returns.
    // TCP hosts, pipe names and COM ports are deployment facts and are never
    // derived from an address.
    //
    // Both verbs guard the address slot, because both parameters are
    // const wchar_t* and nothing else would catch the mistake:
    //   * an empty `toPeer` is E_INVALIDARG (it can never log in -- it nulls
    //     the connection's identity, so every login is refused in silence);
    //   * a `toPeer` containing ':' or '//' is E_INVALIDARG -- that is a
    //     swapped-argument call, and it would otherwise arm a connection to a
    //     URI-shaped address whose every message dies as undeliverable with
    //     no error at arm time;
    //   * a `toPeer` equal to this hub's own address is E_INVALIDARG.
    //
    // Topology.  These verbs are the one place the facade knows both its own
    // address and the peer's, so they classify the pair.  The kernel routes
    // on the dotted address tree but NEVER checks that a connection joins an
    // ancestor to a descendant, and a wrongly-shaped link does not announce
    // itself: it connects, logs in and looks healthy.
    //
    // What a sibling/unrelated edge (e.g. "App.A" <-> "App.B") still does is
    // carry DIRECT traffic both ways -- Send to that peer, and a Broadcast
    // that reaches it -- because RouteP2PeerMsg matches the peer's own address
    // first (P2PeerHub.cpp:723-725).  What it can never do is act as a TRANSIT
    // hop.  Both of the kernel's onward rules require an ancestor/descendant
    // relation (P2PeerHub.cpp:727-739), and the broadcast relay forwards only
    // to children (P2PeerHub.cpp:1235-1237).  So anything addressed BEYOND
    // that peer is silently undeliverable, and a broadcast arriving over the
    // edge stops there instead of propagating into the far side's subtree.
    //
    // Ancestor/descendant pairs (skip levels included) return S_OK; a sibling
    // or unrelated pair is armed anyway but returns P2PF_S_UNRELATED_LINK and
    // raises one OnError.  A `toPeer` that is a P2Padomain PATTERN is not
    // classified -- the far side's real name is unknown until it logs in.
    //
    // S_OK still means "armed", never "the peer is up" -- wait for OnPeerUp.
    virtual HRESULT Listen        ( const wchar_t *toPeer
                                  , const wchar_t *endpoint ) = 0;
    virtual HRESULT Connect       ( const wchar_t *toPeer
                                  , const wchar_t *endpoint ) = 0;

    // --- sending -----------------------------------------------------------
    // Unicast `payload` under `topic` to the hub addressed `dest` (local
    // hub, directly connected hub, or multi-hop across the mesh -- the
    // kernel routes).  `topic` is any client string NOT starting with
    // "P2Pmsg" (kernel-reserved namespace).
    virtual HRESULT Send          ( const wchar_t *dest
                                  , const wchar_t *topic
                                  , const void    *payload
                                  , unsigned int   size ) = 0;

    // Text convenience: sends the UTF-16 string INCLUDING its terminator.
    virtual HRESULT SendText      ( const wchar_t *dest
                                  , const wchar_t *topic
                                  , const wchar_t *text ) = 0;

    // Broadcast `payload` under `topic`: one copy to every peer that is
    // logged in RIGHT NOW, plus onward relay by each receiving hub to its own
    // children (hierarchical addresses only, e.g. "A" -> "A.B" -> "A.B.C").
    // Receivers see it as `broadcast = true` in OnMessage.
    // Returns S_OK if at least one copy was posted, S_FALSE if no peer was up.
    virtual HRESULT Broadcast     ( const wchar_t *topic
                                  , const void    *payload
                                  , unsigned int   size ) = 0;

    // --- properties / lifecycle -------------------------------------------
    // This hub's own address (pointer stable for the hub's lifetime).
    virtual const wchar_t* Address ( ) const = 0;

    // TRUE while the connection to `peer` is logged in.
    virtual BOOL    IsPeerUp      ( const wchar_t *peer ) const = 0;

    // Stop the pump, tear down all connections, then destroy the hub.  The
    // IP2PHub pointer is INVALID afterwards.  Never call from a callback.
    virtual HRESULT Close         ( ) = 0;

    // --- the read side -----------------------------------------------------
    //
    // What a hub can say about itself.  Note what it is NOT: a window onto the
    // kernel.  Every transport keeps its endpoint in protected members with no
    // public getter, so the kernel cannot be asked what wire address a
    // connection used; these methods report what the FACADE wrote down at arm
    // time.  Two consequences worth knowing:
    //   * an endpoint here is what was REQUESTED.  A bind that later failed on
    //     the pump thread is not reflected -- use P2PF_CON_UP for liveness;
    //   * a peer this hub never armed has no endpoint, and reports an empty
    //     one rather than an invented one.  That is the normal case for a
    //     wildcard listener: it arms "*" and then learns real names at login,
    //     so a hub can list more peers than it armed connections.
    //
    // Buffer protocol, uniform across all four (plain Win32, nothing but
    // characters crosses the ABI):
    //   * on entry  *cch is the capacity of `buf` in wchar_t, terminator
    //     included;
    //   * on exit   *cch is the size REQUIRED in wchar_t, terminator included
    //     -- always, whether the call fit or not;
    //   * `buf` may be NULL to ask for the size alone (*cch is then ignored on
    //     entry and the call returns S_OK);
    //   * if the value does not fit, NOTHING is written and the result is
    //     HRESULT_FROM_WIN32(ERROR_MORE_DATA); retry with the size just
    //     reported.
    //
    // Threading: these run on the caller's thread and take the same lock the
    // pump threads use to record peer state, so they are safe from anywhere
    // (a callback included).  But the SET they enumerate is live -- a login on
    // a pump thread can add a peer between two calls -- so `index` is a
    // position in a momentary ordering, never a handle.  Code that needs a
    // consistent picture should take one with Describe().

    // How many peers this hub knows: every one it armed a connection for, plus
    // every one it learned at login.  NOT the count of peers currently up.
    virtual HRESULT GetConCount   ( unsigned int *outCount ) const = 0;

    // One entry, by position.  Either buffer may be NULL to skip that field
    // (both sizes are still reported).  `outFlags` may be NULL.
    // E_INVALIDARG if `index` is past the end.
    virtual HRESULT GetCon        ( unsigned int index
                                  , wchar_t *peerBuf,     unsigned int *peerCch
                                  , wchar_t *endpointBuf, unsigned int *endpointCch
                                  , unsigned int *outFlags ) const = 0;

    // The endpoint this hub armed for one peer, canonically spelled -- so it
    // round-trips straight back into Listen/Connect.  An empty string (and
    // S_OK) for a peer that is known but was never armed by this hub;
    // P2PF_E_UNRESOLVED for a peer this hub has never heard of at all.
    virtual HRESULT GetEndpoint   ( const wchar_t *peer
                                  , wchar_t *buf, unsigned int *cch ) const = 0;

    // The whole picture in one string -- an atomic snapshot, and the form to
    // reach for in a log, a bug report or a script:
    //
    //   address=Demo.Server
    //   con=Demo.Client\ttcp://:7788\tlisten,up,descendant
    //   con=Demo.Peer\t\tup,unrelated
    //
    // One line per peer, tab-separated: peer, endpoint (empty if this hub did
    // not arm it), then comma-separated flags -- listen|dial, up, and exactly
    // one of self|descendant|ancestor|unrelated|pattern.  The format is
    // deliberately dull and line-oriented; it is pinned by a test.
    virtual HRESULT Describe      ( wchar_t *buf, unsigned int *cch ) const = 0;

    // -----------------------------------------------------------------------
    // Reaching past the messaging slice                              (ABI 6)
    //
    // Everything above this line is one subsystem of TargetCore: a hub, its
    // connections, and traffic between them.  These ten are the cheapest of
    // what was left out -- see missing.md, which is the audit they came from.
    // Every one of them is scalars and strings, so nothing here bends the two
    // rules the rest of the header is built on.
    // -----------------------------------------------------------------------

    // Drop ONE peer, leaving the hub and every other connection running.
    //
    // Until this existed, Close() -- whole hub -- was the only way to get rid
    // of a connection, which made "stop talking to that peer" a reason to tear
    // down everything else.  The connection is DESTROYed rather than closed,
    // so a facade dial does NOT start redialling; the peer is forgotten, and
    // Listen/Connect may name it again afterwards.
    //
    // S_OK      the connection is gone (OnPeerDown fires if it was up);
    // S_FALSE   it was signalled but had not gone within ~3 s -- the hub is
    //           still usable, the connection is still retiring, and Close()
    //           will finish retiring it;
    // P2PF_E_NO_PEER      this hub has no connection for that peer;
    // P2PF_E_PUMP_THREAD  called from a callback (it waits; that would
    //                     deadlock the very pump that has to do the work).
    virtual HRESULT Disconnect    ( const wchar_t *peer ) = 0;

    // Register the extended sink.  NULL clears it.  See IP2PHubEvents2 --
    // in particular the substitution rule: from here on OnMessage/OnError go
    // to OnMessageEx/OnEvent instead, so register it BEFORE arming anything.
    virtual HRESULT SetExtEvents  ( IP2PHubEvents2 *events2 ) = 0;

    // Arm a one-shot timer that fires OnTimer(*outTimerId, key) ON THE PUMP
    // THREAD.  This is the only way for client code to run on that thread
    // periodically; before it, a client needed its own thread, which then
    // raced the pump for every piece of state the callbacks touch.
    //
    // IT WILL NOT FIRE EARLY, and it may fire up to about a second LATE.
    // The delay is measured from this call, not from when the pump gets round
    // to arming it.  The late side is the kernel's own granularity: its
    // deadline is `_time64(0)*1000 + delay` (P2Pwin32.cpp:5321) and its poll
    // reads the same one-second clock, so when the pump NOTICES an elapsed
    // timer is quantised in a way the facade cannot reach from outside.  The
    // early side used to be worse than the late one -- a 200 ms timer armed
    // just before a second boundary fired in 5 ms on a hub with any traffic --
    // and is now closed: the facade keeps its own deadline and re-arms rather
    // than deliver ahead of it.
    //
    // So: housekeeping, supervision, timeouts with a second of slack in them.
    // Not pacing, and not anything that needs a tolerance under a second.
    //
    // P2PF_E_NO_SINK if no extended sink is registered: the timer would fire
    // into nothing, and failing at the call is better than at the callback.
    virtual HRESULT SetTimer      ( unsigned int  delayMillisecs
                                  , unsigned int  key
                                  , unsigned int *outTimerId ) = 0;

    // Cancel a timer that has not fired.  S_FALSE if the id is unknown, which
    // is also what a timer that has already fired looks like.
    virtual HRESULT KillTimer     ( unsigned int timerId ) = 0;

    // Run `OnPost(key, context)` on the pump thread, in FIFO order with the
    // hub's own traffic.  Returns as soon as the work is QUEUED.
    //
    // This is the facade's version of the kernel's P2PmsgSink role: the way a
    // GUI thread, a worker or a service handler hands work to a hub without
    // taking a lock against the pump.  `context` is passed through untouched
    // and never dereferenced here -- it is a plain in-process pointer, so it
    // must outlive the callback, and it is the one parameter in this header
    // that no automation tier can carry.
    //
    // P2PF_E_NO_SINK when no extended sink is registered.
    virtual HRESULT Post          ( unsigned int key, void *context ) = 0;

    // Per-connection knobs the kernel keeps on the P2Peerio object hanging off
    // each connection.  ONE pair of methods, `option` selects (P2PF_OPT_*),
    // for the reason there is one pair of arming verbs and not eight.
    //
    // P2PF_E_NO_PEER  this hub has no connection for that peer;
    // P2PF_E_OPTION   unknown option, a Set on a read-only one, or a
    //                 connection with no protocol object to ask.
    virtual HRESULT SetConOption  ( const wchar_t *peer
                                  , unsigned int   option
                                  , unsigned int   value ) = 0;
    virtual HRESULT GetConOption  ( const wchar_t *peer
                                  , unsigned int   option
                                  , unsigned int  *outValue ) const = 0;

    // Round-trip probe.  Blocks until the peer answers or the budget runs out,
    // and reports the round trip in milliseconds.
    //
    // NOT the kernel's P2PmsgPing: that message has no responder at all --
    // P2PeerTarget::On_MsgPing raises P2Pevent_UNKNOWN and returns
    // msgCONTINUE -- so this is a facade-to-facade exchange on a private
    // topic.  A peer that is not a facade hub never answers, and times out.
    // It follows the ordinary routing rules, so a multi-hop peer can be
    // pinged; what comes back is the whole path, not one link.
    //
    // Close() may be called on the hub while this is outstanding: the waiter
    // is released, answers P2PF_E_TIMEOUT, and the close does not return until
    // it is out.  A ping STARTED after a close has begun answers
    // P2PF_E_CLOSED.  (The general rule that no other call may race Close() is
    // unchanged -- this is the one call that had to be safe against it,
    // because it is the one that blocks.)
    //
    // P2PF_E_TIMEOUT      no answer inside `timeoutMillisecs`;
    // P2PF_E_CLOSED       the hub is closing, or closed;
    // P2PF_E_PUMP_THREAD  called from a callback -- the pump cannot deliver
    //                     the answer while it is blocked here.
    virtual HRESULT Ping          ( const wchar_t *peer
                                  , unsigned int   timeoutMillisecs
                                  , unsigned int  *outMillisecs ) = 0;

    // Ask every connection on this hub to close if it is idle (the kernel's
    // P2PsigCon_CLOSEONIDLE).  Returns as soon as the request is queued.
    //
    // There is deliberately no counterpart: the kernel's P2PeerHub::WakeupHub
    // is an ASSERT(0) stub (P2PeerHub.cpp:248-262), so a `resume` here could
    // only be a lie.  A facade dial redials on its own anyway.
    virtual HRESULT CloseIdleCons ( ) = 0;

    // THE ESCAPE HATCH.  Hands back the P2PeerHub this IP2PHub is a face for.
    //
    // Read the rest of this comment before using it.  Everything else in this
    // header exists so that a client needs neither TargetCore's headers nor
    // MFC nor a matching toolset; the moment you cast this pointer you have
    // all three of those requirements back, plus the kernel's own rules about
    // which thread may touch what.  Nothing you do through it is covered by
    // any guarantee this facade makes, and the facade's own bookkeeping (the
    // read side, the peer map) does not learn about it -- post a connection
    // yourself and the hub will not know it exists.
    //
    // It is here because the alternative is worse.  A client that needs one
    // unexposed kernel feature -- a sub-target, a custom P2Peerio, a second
    // pump -- otherwise has to abandon the facade entirely for that one
    // feature.  This turns "impossible" into "your problem", which is the
    // honest trade and the one this layer should be making.
    //
    // Cast to P2PeerHub* (include P2PeerHub.h, link TargetCore.lib).  Valid
    // only until Close().
    virtual HRESULT GetNative     ( void **outNativeHub ) const = 0;

    // -----------------------------------------------------------------------
    // The message model                                              (ABI 7)
    //
    // Send/Broadcast carry a topic and bytes.  The kernel's P2PeerMsg carries
    // rather more, and missing.md §4.2 names the practical consequences of not
    // exposing any of it -- of which the sharpest by far is NO REQUEST/RESPONSE
    // CORRELATION: two replies arriving out of order are indistinguishable, so
    // every client that wanted an answer had to invent a correlation scheme and
    // find room for it inside its own payload.
    //
    // These three close that, plus priority, the bounce control bit, and the
    // destination the read side never reported.  What they do NOT close is the
    // rest of §4.2 -- the P3PmsgItem tree, the Response/Reflect/Redirect
    // factories, wrapping, fragmentation past MAX_PAYLOAD.  That is a design
    // project rather than an afternoon and is deliberately still outstanding.
    // -----------------------------------------------------------------------

    // Send, with the three per-message properties the kernel has and Send
    // could not say.  Identical to Send in every other respect -- same
    // routing, same reserved-topic rules, same MAX_PAYLOAD.
    //
    //   priority  P2PF_PRI_* -- where this message sits in the pump's queue
    //             when more than one is waiting.  P2PF_PRI_DEFAULT leaves it
    //             untouched, which is exactly what Send does.
    //   tag       AN OPAQUE 32-BIT TOKEN THAT TRAVELS WITH THE MESSAGE and is
    //             handed back to the receiver by GetMsgInfo.  The facade never
    //             reads it.  Put a request id in it, answer with the same
    //             value, and correlation costs nothing in the payload -- which
    //             is the whole reason this method exists.
    //             0 is the default and means nothing in particular.
    //   flags     P2PF_SEND_* (currently just P2PF_SEND_NO_BOUNCE).
    virtual HRESULT SendEx        ( const wchar_t *dest
                                  , const wchar_t *topic
                                  , const void    *payload
                                  , unsigned int   size
                                  , unsigned int   priority
                                  , unsigned int   tag
                                  , unsigned int   flags ) = 0;

    // Broadcast, with the same three.  The tag reaches every receiver
    // unchanged, which makes "these twelve replies belong to that one sweep"
    // expressible for the first time.
    virtual HRESULT BroadcastEx   ( const wchar_t *topic
                                  , const void    *payload
                                  , unsigned int   size
                                  , unsigned int   priority
                                  , unsigned int   tag
                                  , unsigned int   flags ) = 0;

    // What else is true of the message being delivered RIGHT NOW.
    //
    // CALL IT FROM INSIDE OnMessage / OnMessageEx, ON THE PUMP THREAD, AND
    // NOWHERE ELSE.  Anywhere else -- another thread, another callback
    // (OnPeerUp, OnTimer, OnPost, OnEvent), or after the delivery returned --
    // it answers P2PF_E_NO_MESSAGE rather than a stale value.  There is no
    // message identity to hold on to and no way to ask about a message later:
    // the kernel owns the object and it is gone by then.  Copy what you need.
    //
    // Why a method here rather than more parameters on a callback: see the
    // ABI 7 note at the top of this header.  It also means an ABI 6 client
    // adds one line inside the handler it already wrote, instead of
    // implementing a third sink interface.
    //
    // Every out parameter is optional (NULL to skip); `destBuf`/`destCch` use
    // the same buffer protocol as the rest of the read side.
    //
    //   dest      the address the message was addressed TO.  Measured, not
    //             assumed: for a unicast this is this hub, and for a broadcast
    //             it is ALSO this hub -- the kernel's relay redirects each copy
    //             to the peer it forwards to (P2PeerHub.cpp, RedirectFactory
    //             in On_P2PeerBCast), so the destination is a last-hop fact,
    //             never the origin's view.  It is reported because the audit
    //             asked for it and because a hub that adopted its name from a
    //             wildcard login has no other way to see what it is called on
    //             the wire; it is NOT a routing history.
    //   priority  a P2PF_PRI_* value -- the kernel's, so a message from a
    //             non-facade peer reports whatever that peer set.
    //   tag       the SendEx tag, 0 if the sender did not set one (or is not
    //             a facade hub).
    //   flags     P2PF_MSG_*.
    virtual HRESULT GetMsgInfo    ( wchar_t *destBuf, unsigned int *destCch
                                  , unsigned int *outPriority
                                  , unsigned int *outTag
                                  , unsigned int *outFlags ) const = 0;

    // -----------------------------------------------------------------------
    // Fields                                                         (ABI 8)
    //
    // Structure in a message without inventing a payload format for it.  See
    // IP2PMessage, and "Named fields" above for why they sit BESIDE the
    // payload rather than in it.
    //
    // What this deliberately does NOT project, and is not a step towards:
    // the kernel's message factories (Response/Reflect/Loopback/Redirect),
    // wrapped messages, and fragmentation past MAX_PAYLOAD.  The first are
    // largely subsumed by ABI 7's correlation tag, which does what a facade
    // client wanted them for; the last is a subsystem -- sequence numbers, a
    // reassembly buffer per sender, and a policy for a sender that dies
    // mid-message -- not a method.
    // -----------------------------------------------------------------------

    // Send a message object.  Identical to SendEx in every other respect --
    // same routing, same reserved-topic rules, same MAX_PAYLOAD on the
    // payload part -- and `msg` is untouched, so the same one may be sent
    // again, to anyone.
    virtual HRESULT SendMsg       ( const wchar_t *dest
                                  , const wchar_t *topic
                                  , IP2PMessage   *msg
                                  , unsigned int   priority
                                  , unsigned int   tag
                                  , unsigned int   flags ) = 0;

    virtual HRESULT BroadcastMsg  ( const wchar_t *topic
                                  , IP2PMessage   *msg
                                  , unsigned int   priority
                                  , unsigned int   tag
                                  , unsigned int   flags ) = 0;

    // The fields on the message being delivered RIGHT NOW.  Same rule as
    // GetMsgInfo, word for word: inside OnMessage/OnMessageEx, on the pump
    // thread, and nowhere else -- P2PF_E_NO_MESSAGE anywhere else, and the
    // bytes are gone when the callback returns, so copy what you keep.
    //
    // `P2PF_MSG_FIELDS` in GetMsgInfo's flags says whether asking is worth
    // it at all; a message with no fields answers a count of 0.
    virtual HRESULT GetFieldCount ( unsigned int *outCount ) const = 0;
    virtual HRESULT GetFieldName  ( unsigned int index
                                  , wchar_t *buf, unsigned int *cch ) const = 0;
    // `size` in is the capacity of `buf` in bytes, out is the size required,
    // always; NULL `buf` asks the size alone; ERROR_MORE_DATA if it does not
    // fit, nothing written.  P2PF_E_NO_FIELD when this message has no field
    // of that name -- which is NOT the same answer as a field that is present
    // and empty (S_OK, size 0).
    virtual HRESULT GetField      ( const wchar_t *name
                                  , void *buf, unsigned int *size ) const = 0;

    // --- driving the pump yourself -----------------------------------------
    //
    // Run ONE turn of this hub's pump on the calling thread.  (ABI 9)
    //
    // Valid only on a hub created with P2PF_HUB_CALLER_PUMPED, and only from
    // the thread that created it -- P2PF_E_NOT_PUMPED and P2PF_E_PUMP_OWNER
    // respectively, which are different mistakes and read as such.
    //
    //   timeoutMillisecs  0 drains what is ready and returns AT ONCE.  This is
    //                     the value a GUI idle handler or WM_TIMER wants: it
    //                     never parks the loop.  A non-zero value parks the
    //                     calling thread for up to that long waiting for work,
    //                     which is what a dedicated loop wants and what a UI
    //                     thread must not do.
    //   outWhat           optional, may be NULL: one P2PF_PUMP_* saying what
    //                     the turn found.  Diagnostic -- loops branch on the
    //                     HRESULT.
    //
    // S_OK      something was dispatched (a message, a connection event, a
    //           timer) -- there may well be more, so call again.
    // S_FALSE   the budget expired with nothing to do.  With a 0 timeout this
    //           is the ordinary answer and means "idle", not "wrong".
    // P2PF_E_CLOSED  the hub is closing or closed; STOP LOOPING.  Once this is
    //           answered it is answered for good.
    //
    // A minimal loop is therefore:
    //
    //     for ( ;; ) { HRESULT hr = hub->Pump(50, 0); if (FAILED(hr)) break; }
    //
    // and a GUI one calls Pump(0,0) until it answers S_FALSE, from wherever it
    // already handles idle.
    //
    // There is deliberately NO waitable handle to hand a
    // MsgWaitForMultipleObjects.  The kernel publishes one -- GetP2PmsgPumpHANDLE
    // -- and for a HUB's pump it is NULL: that event is created only on the
    // named-pump path (P2Pwin32.cpp:2436), never by the hub factory, and a hub
    // pump is woken through its completion port instead (P2PmsgPump::Wakeup,
    // P2Pwin32.cpp:448-458).  Publishing a handle that is always NULL would be
    // worse than not publishing one.  Use GetPending below to keep an idle poll
    // cheap.
    virtual HRESULT Pump          ( unsigned int timeoutMillisecs
                                  , unsigned int *outWhat ) = 0;

    // How many messages are queued on this hub's pump and not yet dispatched.
    //
    // The cheap half of a poll loop: a client that wants to drain "everything
    // waiting" without parking can ask this first and skip the call entirely
    // when it is 0.  It counts the pump's own message FIFO -- not connection
    // activity or completed transport operations, which have no count to give
    // -- so 0 does NOT prove Pump would find nothing.  It is a hint, and the
    // only honest use of it is to avoid work, never to conclude there is none.
    //
    // Unlike Pump this is answerable on any hub and from any thread.
    virtual HRESULT GetPending    ( unsigned int *outCount ) const = 0;

    // Who runs this hub: `outFlags` gets P2PF_PUMP_CALLER_DRIVEN if it was
    // created with P2PF_HUB_CALLER_PUMPED, and P2PF_PUMP_THIS_THREAD if the
    // CALLING thread is the one its pump runs on.  `outThreadId` (optional,
    // may be NULL) gets that thread's id.
    //
    // Both arguments answer for a spawned hub too, which is the use worth
    // having: P2PF_PUMP_THIS_THREAD is exactly the test that decides whether
    // Disconnect or Ping is about to answer P2PF_E_PUMP_THREAD, so library
    // code can ask instead of knowing.
    virtual HRESULT GetPumpInfo   ( unsigned int *outFlags
                                  , unsigned int *outThreadId ) const = 0;

    // --- security -------------------------------------------------------- ABI 11
    //
    // WHAT A SECURE HUB IS, said here because P2PF_HUB_SECURE is one bit in a
    // flags word and this is the only method that bit produces.
    //
    // A hub created with P2PF_HUB_SECURE demands a SIGNED LOGIN, with a
    // per-connection session cypher, from every peer it links to -- on
    // whatever transport is underneath -- and it will not carry a peer it
    // cannot authenticate.  A hub created without it is exactly the hub every
    // ABI up to 10 shipped: it signs nothing and verifies nothing.  There is
    // no third state and no way to change the answer afterwards, because
    // enforcement in TargetCore is hub-wide with no per-connection override:
    // a hub that could be secured later would be one whose existing links
    // silently changed terms, and a hub that could be relaxed later would be
    // one whose secure links silently opened.
    //
    // EVERYTHING BEHIND IT IS ARRANGED FOR YOU, and none of it is in this
    // header:
    //
    //   * an IDENTITY KEY (ECDSA P-256), created on this hub's first run in
    //     the security directory and FOUND on every run after -- a first-run
    //     helper that rotated on restart would change a hub's identity behind
    //     the operator's back.  Its publishable half is written beside it as
    //     "<stem>.key.pub";
    //   * an AGREEMENT KEY (ECDH P-256), the separate key others seal TO,
    //     published as "<stem>.agree.pub".  Deliberately not the same key as
    //     the identity and deliberately unable to be: different container
    //     magic and different entropy, so a swapped or renamed file fails
    //     loudly instead of quietly making a hub sign with the key it agrees
    //     with;
    //   * an ALLOW-LIST, which the hub adds to as it is linked.  This is the
    //     half of provisioning that is not mechanical -- WHO DO YOU TRUST --
    //     and where the answer comes from depends on the verb:
    //       Link      both ends are hubs THIS network owns, in THIS process,
    //                 so the key exchange an operator would do by hand is a
    //                 memcpy.  Both hubs must be secure; a secure hub and a
    //                 plain one cannot be linked, because one of them would
    //                 be demanding a login the other cannot perform.
    //       Listen /  the far end may be in another process, where the facade
    //       Connect   has no way to learn its public points.  They are read
    //                 from the security directory, as the peer's own
    //                 "<stem>.key.pub" and "<stem>.agree.pub" -- the files a
    //                 secure hub publishes for exactly this.  Copying those
    //                 two files from the other machine IS the provisioning
    //                 step, and if they are not there the call is refused
    //                 with P2PF_E_SECURITY naming the file it wanted rather
    //                 than arming something unauthenticated;
    //       a PATTERN  "Demo.*" names no peer whose key could be looked up, so
    //                 it is admitted on a different question: IS THIS HUB
    //                 ALREADY AUTHENTICATING?  The kernel never sees the
    //                 pattern in its auth path -- the allow-list is keyed on
    //                 the source address off the wire and the pattern is an
    //                 accept filter applied to the name the peer CLAIMED -- so
    //                 a hub that requires authentication and lists A and B may
    //                 listen on "Demo.*" and will authenticate exactly those
    //                 two, refusing every stranger the pattern lets through.
    //                 What it may NOT be is a hub's FIRST arm: a hub whose
    //                 only listener is a wildcard has an empty allow-list, has
    //                 therefore never turned enforcement on, and would accept
    //                 anyone -- so that is P2PF_E_SECURITY, saying to link or
    //                 to listen for one named peer first;
    //   * a REVOCATION LIST, shared by every secure hub in the process.  A
    //     position rather than a feature: the file must exist before a hub
    //     that requires authentication will arm, an all-comments file is the
    //     honest "nothing revoked yet", and a configured list that will not
    //     load fails CLOSED and refuses every peer.
    //
    // WHEN ENFORCEMENT GOES ON, which is the one piece of the mechanism worth
    // knowing.  Not at creation: the kernel refuses to START a hub that
    // requires authentication and trusts nobody, because an allow-list that
    // lists nobody refuses everybody, and that refusal is far better at
    // startup than at 3am on the first connection.  So a secure hub is created
    // holding its keys with enforcement off, and turns it on as it takes the
    // first peer it can authenticate -- at which point the kernel's own arming
    // gate is re-run and the connection is armed only if the hub passes it.
    // A secure hub with no peers has nothing to enforce and nothing exposed.
    //
    // WHAT IT DELIBERATELY DOES NOT TURN ON.  TargetCore also defaults to
    // requiring an END-TO-END SEAL on a body that will cross an intermediate
    // hub, and an ORIGIN ATTESTATION on a message arriving down an ancestor
    // link.  Both are properties of an ORIGIN AND A DESTINATION; this flag
    // secures a hub and its EDGES.  For routed traffic -- which is the whole
    // point of Send routing multi-hop and of Link being per edge -- the origin
    // and the destination are two hubs that are not linked to each other and
    // so are not in each other's allow-lists, and requiring either would
    // refuse every routed message and every broadcast on a secure hub.  A
    // silent break dressed as a protection is worse than the honest scope, so
    // the honest scope is what this has: EVERY LINK OF THIS HUB IS
    // AUTHENTICATED AND ENCRYPTED.  The agreement key is provisioned anyway,
    // so a deployment that knows its origin/destination pairs can list them
    // and raise either switch itself through GetNative -- provisioning is the
    // part that cannot be retro-fitted, and a flag is one line.

    // What this hub's security posture IS, asked of the kernel rather than of
    // a record the facade kept.  A cached posture is one forgotten line away
    // from reporting a state the hub does not have, which for this particular
    // question is the whole failure mode.
    //
    //   buf/cch    this hub's identity FINGERPRINT -- the human-checkable form
    //              of its public point, which is what an operator reads down a
    //              phone line to confirm the key that arrived is the key that
    //              was sent.  Empty for a hub that holds no identity, which is
    //              every hub not created with P2PF_HUB_SECURE.  The usual
    //              buffer protocol (*cch in = capacity in characters, out =
    //              size required INCLUDING the terminator, NULL buffer = size
    //              query, short buffer = ERROR_MORE_DATA with nothing
    //              written).  Both may be NULL if only the flags are wanted.
    //   outFlags   any OR of P2PF_SEC_*, or 0 for a plain hub.  Optional.
    //
    // A FINGERPRINT IS NOT AN IDENTIFIER THIS CODE TRUSTS, and nothing here
    // treats it as one -- a trust decision is made against the full public
    // point, in the allow-list.  It is for a human to compare.
    virtual HRESULT GetSecurityInfo ( wchar_t *buf, unsigned int *cch
                                    , unsigned int *outFlags ) const = 0;

  protected:
    ~IP2PHub ( ) { }            // destroyed only via Close()/network Release()
};

// ---------------------------------------------------------------------------
// IP2PNetwork -- the ONE init object.  Owns kernel startup/shutdown
// (StartupP2Pmsg + WSAStartup internally -- the caller never touches either)
// and every hub it created.
// ---------------------------------------------------------------------------
struct IP2PNetwork
{
    // Create a hub with its own address and event sink, and start it (the
    // pump thread is spawned before this returns).  `events` must outlive
    // the hub.  On success *outHub is owned by the network: release it with
    // (*outHub)->Close() or leave it to Release().
    //
    // `address` must be unique among the hubs THIS PROCESS currently has
    // open.  Two live hubs sharing one address corrupt the kernel's hub
    // registry -- logins on unrelated hubs start being refused and the
    // process dies shortly after -- and the kernel does not police it, so
    // this does: P2PF_E_HUB_DUPLICATE, nothing created.  Comparison is
    // case-sensitive, like every other P2Paddr comparison.  Closing a hub
    // frees its address for reuse.
    virtual HRESULT CreateHub ( const wchar_t  *address
                              , IP2PHubEvents  *events
                              , IP2PHub       **outHub ) = 0;

    // Drop the network reference obtained from P2PF_CreateNetwork.  When the
    // LAST reference goes, every remaining hub is Close()d and the kernel is
    // shut down (CleanupP2Pmsg + WSACleanup).
    virtual ULONG   Release   ( ) = 0;

    // Facade + kernel build tag, for logs ("TargetFacade 2 / TargetCore ...").
    virtual const wchar_t* VersionString ( ) const = 0;

    // --- linking -----------------------------------------------------------
    //
    // Arm BOTH ends of one edge between two hubs THIS network owns, in the
    // order that works.  The network is the only object that holds every live
    // hub in the process, so it is the only one that can do this; a client
    // doing it by hand has to arm the listening side first, and with an
    // explicit dmx:// or serial:// endpoint that ordering is load-bearing --
    // those dials are one-shot.  Link removes that folklore from every
    // in-process call site:
    //
    //     net->Link ( L"Demo", L"Demo.Client", NULL );
    //
    // THE ARGUMENTS ARE NOT INTERCHANGEABLE.  `listenerAddr` names the hub
    // that gets the passive endpoint, `dialerAddr` the hub that gets the
    // active one; the roles are observable (which side redials, and which
    // side an accepted connection is spawned on).  Swapping them is legal and
    // gives a working but differently-shaped link.
    //
    // `endpoint` NULL/empty selects an in-process Dmx service under a name
    // derived from the ordered pair -- zero configuration, and unique per
    // DIRECTED edge, so one hub may be linked to many peers.  Otherwise it is
    // any endpoint Listen/Connect accept, in its DIAL form: the dialer
    // gets it verbatim and the listener gets it with the host dropped
    // ("tcp://10.0.0.7:7788" listens as "tcp://:7788").
    //
    // Link is per EDGE, not per hub, and it is not a replacement for the
    // arming verbs:
    //   * both hubs must already exist -- there is no way to say "expect this
    //     peer whenever it turns up", which is what a service does; use
    //     Listen for that;
    //   * a hub with an in-process sibling AND a TCP link to another machine
    //     arms the second one itself;
    //   * for a chain A-B-C, link A-B and B-C only.  Send() already routes
    //     multi-hop, so linking every pair costs connections and buys
    //     nothing.
    //
    // Failure is atomic in every case it can pre-detect (unknown address,
    // duplicate peer on either hub, unparseable endpoint): nothing is armed.
    // If the dialer still fails after the listener armed, the listener's
    // connection is retracted; P2PF_E_LINK_PARTIAL is returned in the one
    // case where that retraction itself did not complete, and names the hub
    // left armed (`listenerAddr`, expecting `dialerAddr`) rather than leaving
    // it to be discovered.
    //
    // Returns P2PF_E_NO_HUB if either address is not a live hub of this
    // network, P2PF_E_CON_DUPLICATE if either side already has a connection
    // for the other (calling Link twice for one pair is an error, not a
    // no-op), and P2PF_S_UNRELATED_LINK -- a SUCCESS code -- if the two
    // addresses are siblings or unrelated (see IP2PHub "Topology").
    //
    // SECURE HUBS ARE LINKED THE SAME WAY, and this is the one call in the
    // ABI that can complete the trust exchange by itself: both ends are hubs
    // this network owns, in this process, so putting each hub's public points
    // into the other's allow-list is a memcpy rather than an operator and two
    // published files.  Both hubs must have been created with P2PF_HUB_SECURE
    // or neither must -- a secure hub demands a signed login that a plain one
    // cannot perform, so the mixed pair is P2PF_E_SECURITY with nothing armed
    // rather than a link that quietly never comes up.  (ABI 11)
    virtual HRESULT Link ( const wchar_t *listenerAddr
                         , const wchar_t *dialerAddr
                         , const wchar_t *endpoint ) = 0;

    // --- the deployment map ------------------------------------------------
    //
    // WHERE THE ENDPOINT STRING COMES FROM.  Making the endpoint a string was
    // half the point of the arming pair; this is the other half.  Until these
    // existed the only place to put that string was the call site, so
    // "endpoint as configuration" was true of the type and false of the API --
    // nothing could read an ini, an argv or a registry value into arming.
    //
    //     net->SetEndpointMap ( ReadWholeFile ( L"peers.ini" ), NULL );
    //     hub->Listen  ( L"Demo.Client", NULL );   // no endpoint anywhere
    //     hub->Connect ( L"Demo.Server", NULL );   // in the code
    //
    // ONE TABLE, KEYED BY ADDRESS, holding the DIAL form:
    //
    //     # where each address lives.  '#' and ';' comment, blanks ignored.
    //     Demo.Server = tcp://10.0.0.7:7788
    //     Demo.Edge   = pipe://P2PmsgEdge
    //
    // It is keyed by address rather than by (hub, peer) because "where does
    // Demo.Server live" is a fact about Demo.Server, not about who is asking.
    // That is what lets one table serve both roles: a Connect with no endpoint
    // looks up its PEER, and a Listen with no endpoint looks up THIS HUB'S OWN
    // address and drops the host ("tcp://10.0.0.7:7788" listens as
    // "tcp://:7788", the same conversion Link performs).  So the same file
    // deploys to every machine and each process just creates its hubs.
    //
    // PRECEDENCE.  An omitted endpoint resolves in two tiers: (1) this map,
    // (2) the in-process convention -- a derived Dmx service on a Listen, a
    // live sibling hub's own record on a Connect.  What a deployment STATED
    // beats what a process can INFER.  The map is empty until you fill it, so
    // a client that never calls these behaves exactly as it always did.
    //
    // Link does NOT consult the map: both its ends are hubs this network owns,
    // in this process, so a configured cross-machine endpoint would be
    // answering a question nobody asked.
    //
    // A map entry is an endpoint a HUMAN WROTE, so it is treated like one
    // typed at the call site -- validated when it is set, and dialled once
    // rather than retried, because it can be wrong rather than merely early.
    //
    // Set or clear ONE entry.  An empty/NULL `endpoint` REMOVES it (empty
    // already means "resolve this for me" everywhere else in this ABI).
    // The endpoint is validated now, in its DIAL form -- so a tcp entry must
    // name a host -- and P2PF_E_ENDPOINT comes back here rather than from an
    // arming call in another file three hours later.  E_INVALIDARG for an
    // empty `address`, or one containing ':' or "//" (that is an endpoint in
    // the address slot).
    virtual HRESULT SetEndpoint    ( const wchar_t *address
                                   , const wchar_t *endpoint ) = 0;

    // REPLACE the whole map from one block of text -- an ini file, a resource,
    // a here-doc, argv joined by newlines.  Replace rather than merge: a map
    // is a statement of what the world looks like, and re-reading a file must
    // not leave a deleted line working until the next restart.  NULL or empty
    // text therefore CLEARS the map.
    //
    // Format: one `address = endpoint` per line; blank lines and lines whose
    // first non-space character is '#' or ';' are ignored; whitespace around
    // both sides is trimmed; the first '=' splits (a pipe or Dmx name may
    // contain more).
    //
    // ALL-OR-NOTHING.  On P2PF_E_ENDPOINT nothing is applied -- the previous
    // map is exactly as it was -- and `*badLine` (optional, may be NULL) gets
    // the 1-based line number, counting blanks and comments so it is the
    // number your editor shows.  Rejected: a line with no '='; an empty or
    // endpoint-shaped address; anything the endpoint grammar refuses; the same
    // address twice; and serial://, for the reason Link refuses it -- a
    // null-modem link is two DIFFERENT local ports and one entry cannot say
    // that.
    virtual HRESULT SetEndpointMap ( const wchar_t *text
                                   , unsigned int  *badLine ) = 0;

    // What the map says about `address`, canonically spelled, through the same
    // caller-sized buffer protocol as the hub's read side (*cch in = capacity,
    // out = size required, NULL buffer = size query, short buffer =
    // ERROR_MORE_DATA with nothing written).  P2PF_E_UNRESOLVED when there is
    // no entry.
    //
    // This answers what the MAP SAYS, not what a hub armed -- IP2PHub::
    // GetEndpoint is the other question, and the two agree only when the map
    // is what the resolution actually used.
    virtual HRESULT GetEndpointFor ( const wchar_t *address
                                   , wchar_t *buf
                                   , unsigned int *cch ) const = 0;

    // --- messages ----------------------------------------------------------
    //
    // Make an empty message to fill in and hand to IP2PHub::SendMsg.  (ABI 8)
    //
    // On the NETWORK rather than on a hub, because an IP2PMessage belongs to
    // no hub -- it holds no reference to one, and the same object may be sent
    // by any hub, repeatedly, from any thread.  Putting the factory on a hub
    // would imply a coupling that does not exist, and would raise a question
    // about what happens when that hub closes, which does not arise.
    //
    // Release it when done; it is not refcounted and nothing else holds it.
    virtual HRESULT CreateMessage  ( IP2PMessage **outMessage ) = 0;

    // --- choosing the thread -----------------------------------------------
    //
    // CreateHub, plus one word about WHOSE THREAD the hub runs on.  (ABI 9)
    //
    // `flags` is any OR of P2PF_HUB_CALLER_PUMPED and P2PF_HUB_SECURE, or
    // P2PF_HUB_SPAWN_PUMP (0) for neither; everything else about the call,
    // including every failure it can report, is CreateHub exactly.
    // CreateHubEx(addr, events, 0, out) and CreateHub(addr, events, out) are
    // the same call, which is why CreateHub is not deprecated and not
    // reimplemented -- it forwards here.
    //
    // With P2PF_HUB_CALLER_PUMPED the hub is created ON THIS THREAD and is
    // inert until this thread calls IP2PHub::Pump.  Read "Who runs the pump"
    // above before using it: the thread affinity that buys is also a promise
    // the client is now making, and Close is one of the calls that has to keep
    // it.
    //
    // ONE PER THREAD.  The kernel keys a pump by thread id and refuses a
    // second one, so a thread that already owns a caller-pumped hub gets
    // P2PF_E_HUB_SPAWN for the next -- as does a thread that is itself some
    // other hub's pump (a callback, in other words).  A process may hold as
    // many caller-pumped hubs as it has threads to give them.
    //
    // With P2PF_HUB_SECURE the hub holds an identity and demands a signed
    // login from every peer it links to; read IP2PHub::GetSecurityInfo, which
    // is where the whole of that is written down.  The two flags are
    // orthogonal -- either kind of hub can be secure -- and the key material
    // is made here, before the pump exists, so a directory that cannot be
    // written or a key file that cannot be loaded is P2PF_E_SECURITY with the
    // file named on the diagnostic stream, and NO HUB IS CREATED.  A client
    // that wants the keys somewhere particular calls SetSecurityDir first.
    virtual HRESULT CreateHubEx    ( const wchar_t  *address
                                   , IP2PHubEvents  *events
                                   , unsigned int    flags
                                   , IP2PHub       **outHub ) = 0;

    // --- diagnostics ---------------------------------------------------- ABI 10
    //
    // Subscribe to the kernel's own event stream.
    //
    // READ "Diagnostics" ABOVE FIRST.  Three of its five points change how a
    // handler must be written: this is the one callback in the ABI that is NOT
    // delivered on a pump thread, it can run on several threads at once, and
    // registering displaces the kernel's own file logger because the process
    // has exactly one slot for this.
    //
    //   sink   the object to deliver to.  NOT owned: it must outlive the
    //          network, or be taken back with SetDiagSink(NULL, 0).  A second
    //          call REPLACES the first; there is one sink, process-wide.
    //   mask   any OR of P2PF_DIAGM_*.  Filtered by the facade before your
    //          handler is called (point 4 above -- it does not save the kernel
    //          any work, only you).
    //
    // NULL `sink` unregisters, and unregistering is not merely tidy: the slot
    // holds a raw pointer to your object, so a sink freed while registered is
    // a crash on the next event the kernel raises.  Releasing the network
    // unregisters too, whatever the client did.
    //
    // Unlike every other registration in this ABI this one takes effect
    // IMMEDIATELY, on the calling thread -- there is no pump in the path to
    // marshal through, which is the same fact that makes delivery synchronous.
    //
    // E_INVALIDARG for a mask with the kernel's reserved top bit set.
    virtual HRESULT SetDiagSink    ( IP2PDiagEvents *sink
                                   , unsigned int    mask ) = 0;

    // Narrow or widen the subscription without replacing the sink.  S_FALSE --
    // not a failure -- when no sink is registered, because "filter nothing" is
    // a coherent thing to have asked for and an error would make every caller
    // check whether it had armed one yet.
    //
    // A severity gate is this method: register once with P2PF_DIAGM_PROBLEMS
    // and widen to P2PF_DIAGM_ALL when a user ticks "verbose".  The change is
    // seen by the next event raised on any thread.
    virtual HRESULT SetDiagMask    ( unsigned int mask ) = 0;

    // The mask currently in force, or 0 when no sink is registered.
    virtual HRESULT GetDiagMask    ( unsigned int *outMask ) const = 0;

    // The parts of the event being delivered that OnDiag has no argument for.
    // `part` is one P2PF_DIAGT_* value; the buffer protocol is the usual one
    // (*cch in = capacity in characters, out = size required INCLUDING the
    // terminator, NULL buffer = size query, short buffer = ERROR_MORE_DATA
    // with nothing written).
    //
    // VALID ONLY INSIDE OnDiag, ON THE THREAD IT WAS DELIVERED TO, exactly as
    // GetMsgInfo is valid only inside a delivery: outside one it answers
    // P2PF_E_NO_DIAG rather than an empty string, because "the kernel attached
    // no advice" and "you asked in the wrong place" must not both read as "".
    // The gate is per-thread rather than per-object here, because unlike a
    // message delivery there can be several of these running at once.
    //
    // E_INVALIDARG for an unknown `part`.
    virtual HRESULT GetDiagText    ( unsigned int part
                                   , wchar_t *buf, unsigned int *cch ) const = 0;

    // The numeric half of the same question, under the same rule.  Every
    // argument is optional -- pass NULL for what you do not want.
    //
    //   outHResult   the HRESULT the raiser attached, or S_OK when it attached
    //                none.  Most events carry none; the ones that do are worth
    //                the column.
    //   outTime      when the event was CREATED, as seconds since the Unix
    //                epoch.  Stamped by the kernel at creation rather than at
    //                delivery -- which for an event that sat in a throw/catch
    //                chain is not the same instant, and the difference is
    //                itself diagnostic.
    //   outThreadId  the thread the event was raised ON, which is also the
    //                thread your handler is running on.  This is the column
    //                that makes a kernel log readable: a hub pump, a client
    //                thread and a transport completion all narrate into one
    //                stream and nothing else tells them apart.
    //
    // P2PF_E_NO_DIAG outside an OnDiag delivery.
    virtual HRESULT GetDiagInfo    ( unsigned int *outHResult
                                   , unsigned int *outTime
                                   , unsigned int *outThreadId ) const = 0;

    // Write one event into the same stream, from any thread.
    //
    // The other half of a diagnostics surface, and the cheaper half to
    // overlook: a client that can read the kernel's narration but cannot add
    // to it ends up keeping two logs and interleaving them by hand.  This goes
    // to the same slot -- so to this sink, and to the kernel's file logger
    // instead if that is the one holding it.
    //
    //   severity  any P2PF_DIAG_* value.  P2PF_DIAG_APP is the class nothing
    //             in the kernel raises, so it is the one to use unless you
    //             mean to be filtered alongside the kernel's own errors.
    //   module    what raised it -- a function name, a subsystem, anything.
    //             Optional; empty is fine.
    //   text      the sentence.  Required.
    //
    // Raised events are NOT displayed: the kernel's own disposal path would
    // print an ERROR to the console or, in a window-less process, put it in a
    // message box on whatever thread raised it.  This one only notifies.
    //
    // S_OK      raised.
    // S_FALSE   deliberately suppressed: this thread is INSIDE an OnDiag
    //           delivery, and a logger that logs from its own log handler is
    //           the one feedback loop this facade can close.  Not an error --
    //           a handler that formats a line and occasionally raises one of
    //           its own should not have to know which it is in.
    // E_INVALIDARG for an empty `text` or a severity above 31.
    virtual HRESULT RaiseDiag      ( unsigned int   severity
                                   , const wchar_t *module
                                   , const wchar_t *text ) = 0;

    // Is anything listening for these classes right now?
    //
    // `mask` is any OR of P2PF_DIAGM_*; `*outMatched` gets the subset of it a
    // registered sink has asked for.  0 means nobody is.
    //
    // What it is FOR is skipping work.  A trace line that costs a format, a
    // concatenation and a timestamp should not be built when nothing will read
    // it:
    //
    //     unsigned int wanted = 0;
    //     if ( SUCCEEDED(net->IsDiagWanted(p2pf::P2PF_DIAGM_TRACE, &wanted))
    //          && wanted )
    //       net->RaiseDiag ( p2pf::P2PF_DIAG_TRACE, L"Sweep", Expensive() );
    //
    // It answers about THIS FACADE's sink.  The kernel publishes its own
    // IsP2PeventReg for this question and it is not usable: it reports on a
    // second sink registry that nothing in the kernel ever notifies, so it
    // answers 0 for a process whose logging is working perfectly.
    //
    // A hint about this instant, not a lock -- a sink may register or narrow
    // immediately afterwards.  Use it to avoid work, never to conclude that
    // something was or was not logged.
    virtual HRESULT IsDiagWanted   ( unsigned int  mask
                                   , unsigned int *outMatched ) const = 0;

    // --- security -------------------------------------------------------- ABI 11
    //
    // Where every secure hub of this process keeps its key material.
    //
    // THE ONLY SECURITY CALL ON THE NETWORK, and it is here rather than on a
    // hub because the directory is the PROCESS'S: the revocation list in it is
    // shared by every secure hub, since revoking a key is an operator editing
    // one file and a per-hub file would mean doing that once per hub and
    // getting it wrong once per hub.
    //
    // OPTIONAL.  NULL or empty restores the default, which is a "p2p"
    // directory beside the loaded module -- resolved from the module's own
    // path rather than from the working directory, so a copy of the tree
    // elsewhere just works and a stale key from another build tree is never
    // picked up silently.  The DLL's path and not the executable's: a COM
    // server is loaded by whatever host CoCreates it, and keys that followed
    // the host would be a different identity per host.  The directory is
    // created if it is not there, ONE level; a deeper path named here must
    // already exist, because quietly building a tree of directories for a
    // mistyped path is worse than the refusal.
    //
    // Call it BEFORE the first hub created with P2PF_HUB_SECURE.  Afterwards
    // it answers P2PF_E_SECURITY: that hub holds keys loaded from the old
    // directory, and a call that appeared to move them but did not would be
    // worse than a refusal.  P2PF_E_SECURITY too for a path this process
    // cannot create.
    virtual HRESULT SetSecurityDir  ( const wchar_t *dir ) = 0;

  protected:
    ~IP2PNetwork ( ) { }
};

} // namespace p2pf

// ---------------------------------------------------------------------------
// Factory -- the only exported symbol a client needs.
//
// Process-wide refcounted singleton: every successful call returns the SAME
// network (kernel startup happens on the first call only) and adds one
// reference; pair each call with exactly one Release().
//
// `abiVersion` MUST be p2pf::ABI_VERSION from the header you compiled
// against; a stale client gets P2PF_E_ABI_MISMATCH instead of a vtable skew.
// Accepted: ABI_VERSION_MIN (4) through ABI_VERSION (11).  4 is the floor
// because it removed vtable slots and moved every method after the arming
// pair, leaving no compatible prefix for anything older; every version since
// has only APPENDED (5 to IP2PNetwork, 6 and 7 to IP2PHub, 8, 9, 10 and 11 to
// both), so a v4 through v10 binary runs here unchanged -- it simply cannot
// see the newer methods.
// ---------------------------------------------------------------------------
extern "C" P2PF_API HRESULT __stdcall
P2PF_CreateNetwork ( unsigned int abiVersion, p2pf::IP2PNetwork **outNetwork );
