# TargetFacade

A minimal, macro-free facade DLL over `TargetCore.dll`. Clients include
**one header** (`include/TargetFacade.h`), link **one import lib**, and never
see P2PeerHub, P2PeerCon, P2PeerMsg, MFC, WSAStartup or any `BEGIN_*_MAP`
macro.

```
  C++ client ─────────> TargetFacade.dll ──> TargetCore.dll ──> Msgcore.dll
                             ^  (flat vtable ABI, HRESULT)   (MFC classes, factories, maps)
                             │
script / VB / .NET ──> TargetCom.dll
                       (dual interfaces, IDispatch, connection-point events;
                        optional, lives in com\ -- see below)
```

## Status: complete and tested

Everything in the public header is implemented. `test/FacadeSmokeTest` is a
console client that includes **only** the facade's public headers — no MFC, no
`afx*`, no TargetCore, no WinSock — so it fails to compile if the facade ever
starts leaking its internals.

181 checks, Debug|x64 and Release|x64, all green: TCP loopback (both sides see
`OnPeerUp`), unicast text, 256-byte binary round-trip, broadcast with its
topic intact, `IsPeerUp`, in-process Dmx, named pipe, reserved-topic,
duplicate-peer and duplicate-hub-address rejection (with address reuse after
close, and the refused duplicate leaving the original arm's record intact),
clean teardown — plus the one arming pair over every transport, all
fourteen endpoint-grammar rejections, the peer guards, the topology
classification, in-process endpoint resolution, `IP2PNetwork::Link` with its
whole failure contract, the read side (`GetConCount`/`GetCon`/`GetEndpoint`/
`Describe`, the per-peer routing relation, and the caller-sized buffer
protocol), the two Dmx regressions in sections 16 and 17, the deployment map
(18), a three-hub chain built from text (19), the login accept filter (20), and
the ten ABI 6 methods (21), their boundaries (22), the ABI 7 message model
(23–24), ABI 8's named fields (25), ABI 9's caller-driven pumping (26) and
ABI 10's diagnostics (27), and ABI 11's secure hubs (14b) — see "Past the
messaging slice", "The message model", "Named fields", "Who runs the pump",
"The kernel narrates" and "Secure hubs" below.

```
cd test\FacadeSmokeTest\x64\Debug && FacadeSmokeTestd.exe    # exit 0 = pass
```

Serial (`serial://COM5`) is not covered by this smoke test — it needs two real
or virtual (com0com) COM ports on a null-modem link, so it is skipped on a
machine without them. It **is** exercised end-to-end, on both configurations,
by `_TargetCore_UseExamplesLight\Com232MeshTest` against a com0com COM5↔COM6 pair.

`test/WildcardListenTest` is a second console client, same rules, covering what
`toPeer` means on the listening side: 47 checks, Debug|x64 and Release|x64, all
green (see "What `toPeer` means on `Listen`" below).

```
cd test\WildcardListenTest\x64\Debug && WildcardListenTestd.exe   # exit 0 = pass
```

`test/SecureWildcardTest` is a third, and the only one that is **two
processes**: it puts a stranger in front of a secure hub's wildcard listener
over a real TCP socket, which is the one question about `P2PF_HUB_SECURE` a
single process cannot answer — both ends of a `Link` are hubs the facade
provisions itself, so in-process it can only ever produce peers that are
already trusted. 15 checks (see "Two processes, one stranger" below).

```
cd out\x64\Debug && SecureWildcardTest.exe                 # exit 0 = pass
```

The optional COM layer in `com\` is built and tested too: 101 more checks from a
real STA client, both configurations, plus 27 late-bound from PowerShell (see
"`com\` — the ATL layer" below).

The facade→TargetCore mapping is documented in `src/FacadeInternal.h`.

## The API in 20 lines

```cpp
#include "TargetFacadeFn.hpp"   // or the raw TargetFacade.h if you prefer

p2pf::Network net;                                   // ONE init: StartupP2Pmsg + WSAStartup inside
p2pf::Hub server = net.createHub(L"Demo");           // P2PeerHub + SpawnHub inside
server.onTopic(L"chat", [](const p2pf::Message& m) { // no BEGIN_P2PeerMsg_MAP
    wprintf(L"%s: %s\n", m.source, m.text());
});
server.onPeerUp([](const wchar_t* p){ wprintf(L"up %s\n", p); });
server.listen(L"Demo.Client", L"tcp://:7788");       // ConWsa::ServiceFactory + PostP2PeerCon inside

p2pf::Hub client = net.createHub(L"Demo.Client");
client.connect(L"Demo", L"tcp://127.0.0.1:7788");    // ConWsa::ClientFactory + PostP2PeerCon inside
client.sendText(L"Demo", L"chat", L"hello");         // P2PeerMsg32 + PostP2PeerMsg inside
```

(The hub is called `Demo` and its peer `Demo.Client` — parent and child, not
two siblings. The arming verbs classify the pair, and a sibling link is a real
topology problem they report; see "The topology check the kernel never makes".)

## One pair of verbs for every transport

There used to be eight arming methods — `Listen`/`Connect`, `ListenPipe`/
`ConnectPipe`, `ListenDmx`/`ConnectDmx`, `ListenSerial`/`ConnectSerial`. They
were one method plus a four-way switch on how the endpoint is spelled: the same
`AFX_MANAGE_STATE`, the same null check, the same `PostCon` tail, differing
only in which kernel factory got called and what was stuffed into it. So they
are now **one** pair that takes the endpoint as a value, matching the single
`Send`:

```cpp
hub.listen  ( L"Demo.Client", L"tcp://:7788"           );
hub.connect ( L"Demo",        L"tcp://127.0.0.1:7788"  );
hub.listen  ( L"Demo.Client", L"pipe://P2PmsgDemo"     );
hub.connect ( L"Demo",        L"dmx://DemoService"     );
hub.listen  ( L"Demo.Client", L"serial://COM5"         );
```

| scheme | listen form | connect form | retries? |
|---|---|---|---|
| `tcp` | `tcp://:7788` | `tcp://127.0.0.1:7788` | yes |
| `pipe` | `pipe://name` | `pipe://name` | yes |
| `dmx` | `dmx://service` | `dmx://service` | no |
| `serial` | `serial://COM5` | `serial://COM5` | no |
| *(omitted)* | `NULL` / `""` | `NULL` / `""` | yes, 8 attempts |

Retry is a property of the *endpoint*, not of the method name — and the last row
is the one that took longest to get right (see "the derived Dmx dial" below).
An endpoint the caller **typed** and the facade **derived** fail for different
reasons, so they fail differently. The omitted form does not have to mean
"in-process", either: it is resolved against a **deployment map** first, so a
whole topology can come from a text file with no endpoint in the code at all.

The grammar is parsed **inside the DLL**, which is the whole point: an
endpoint becomes a configuration *value* — an ini entry, a registry value, an
argv element — that changes transport with no rebuild anywhere, and a fifth
transport becomes a parser entry rather than two more vtable slots, two more
IDL dispids and two more wrappers per language binding. A listen host must be
empty, `*` or `0.0.0.0` (the kernel binds `INADDR_ANY` unconditionally);
`tcp://[::1]:7788` is **rejected**, not mangled, because `P2PeerConWsa` is
`AF_INET` throughout; ports and COM numbers are range-checked in one place
instead of being silently narrowed to a `short`. Everything malformed is
`P2PF_E_ENDPOINT` at the call site, before anything is manufactured — so a bad
endpoint never burns the one connection slot the kernel allows per peer
address.

The eight typed verbs are **gone**, not deprecated-and-kept, and their names
were reused: what shipped as `ListenEx`/`ConnectEx` is now simply
`Listen`/`Connect`. The `Ex` suffix only ever existed because an append-only
ABI could not reuse a name the old vtable still held; with the old slots
removed there is nothing left to avoid, and the primary verbs of the API
should not carry a suffix for the rest of their life. That made ABI **4** a
hard cut; ABI **5** went back to appending — see "Design decisions" below.

### The address as the whole handle

Omit the endpoint (`nullptr` or `L""`) and the facade resolves it — but only
where a process genuinely can:

```cpp
server.listen  ( L"Demo.Client", nullptr );   // in-process, zero configuration
client.connect ( L"Demo",        nullptr );
```

A listen arms an in-process Dmx service under a name derived from the ordered
pair *(listener, dialer)*; a dial looks for a live hub of that address **in
this process** which has already armed a listener expecting us, and dials
whatever that hub armed. The pair is in the derived name because the kernel's
Dmx rendezvous matches on the service string alone (`P2PeerConDmx.cpp:646-655`
— first `SERVICE` connection with an equal name wins, peer address and owning
hub ignored), so a name keyed on one address would cross-wire a hub expecting
two peers.

Everything else fails with `P2PF_E_UNRESOLVED`, deliberately. A TCP host, a
pipe name and a COM port are deployment facts; no software can *derive* them
from `"Demo"`, and there is no fallback that dials a derived name at a
sibling which has not armed yet — that would always return `S_OK` and then
lose the rendezvous in silence, because an explicit `dmx://` dial is one-shot
and a dial that never connects fires no `OnPeerDown` (the peer was never up).
An error at the caller that made the mistake beats a connection that quietly
never happens.

Deployment facts cannot be derived — but they can be *stated*, which is what
the map below is for.

### The deployment map — an endpoint that is nowhere in the code

Making the endpoint a string was half of what the arming pair was for. This is
the other half. Until this existed the only place to put that string was the
call site, so "the endpoint is configuration" was true of the *type* and false
of the *API*: nothing could read an ini, an argv or a registry value into
arming.

```cpp
net.setEndpointMap ( readWholeFile ( L"peers.ini" ) );

server.listen  ( L"Demo.Client", nullptr );   // no endpoint anywhere
client.connect ( L"Demo.Server", nullptr );   // in the code
```

```ini
# peers.ini -- where each address lives.  '#' and ';' comment; blanks ignored.
Demo.Server = tcp://10.0.0.7:7788
Demo.Edge   = pipe://P2PmsgEdge
```

**One table, keyed by address, holding the dial form.** Keyed by address rather
than by *(hub, peer)* because "where does `Demo.Server` live" is a fact about
`Demo.Server`, not about who is asking — and that is exactly what lets one
table serve both verbs. A `connect` looks up its **peer**; a `listen` looks up
**its own hub's address** and drops the host (`tcp://10.0.0.7:7788` listens as
`tcp://:7788`, the same conversion `Link` performs on the endpoint it is
handed). So the same file deploys to every machine and each process just
creates the hubs it owns.

**Precedence: what a deployment *stated* beats what a process can *infer*.** An
omitted endpoint resolves in two tiers — (1) this map, (2) the in-process
convention above. The map is empty until you fill it, so a client that never
calls these behaves exactly as it always did. `Link` does **not** consult it:
both its ends are hubs this network owns, in this process, so a configured
cross-machine endpoint would be answering a question nobody asked.

**Validated where the mistake was made.** Endpoints are parsed when the map is
set, not when something later tries to arm with them, and a bad block is
refused **whole** — the previously loaded map is untouched — with the 1-based
line number, counting blanks and comments so it is the number your editor
shows. That matters most one tier up, where `Err.Description` reads
*`SetEndpointMap ( 'line 2' ) failed: the endpoint could not be parsed…`*
instead of a `Connect` failing an hour later about a file it never mentions.
Refused: a line with no `=`; an empty or endpoint-shaped address; anything the
grammar rejects; the same address twice; and `serial://`, for the reason `Link`
refuses it — a null-modem link is two *different* local ports and one entry
cannot say that.

**A map entry is a string a human wrote**, so it is treated like one typed at
the call site: dialled once rather than retried, because it can be *wrong*
rather than merely *early*. That is the same `bDerived` distinction the retry
tier turns on — see "the derived Dmx dial" below.

This arrived as **ABI 5**, which is append-only: three methods on the end of
`IP2PNetwork`, nothing moved, `IP2PHub` untouched. So the factory accepts 4 as
well and a binary built against the previous header still runs — pinned by a
check, since that is the one property of an append-only change that can quietly
stop being true.

For cross-machine work the better framing is still the kernel's own: `Send`
already routes multi-hop across the mesh, so you rarely need a direct link to a
particular hub — you need *a* link. One armed edge to any node reaches the
rest, and the map is how that one edge stops being a compile-time constant.

### A whole process from one block of text

`include/TargetFacadeTopology.hpp` is **optional and header-only**, and that is
the design rather than a shortcut: it adds nothing to the ABI and is composed
entirely from `CreateHub`, `SetEndpointMap` and `Link`, which is exactly what
the plan recommended for it. It waited for the map, because the map is the half
that had to live inside the DLL.

```
# a three-hub chain: Top -> Top.Mid -> Top.Mid.Leaf
hub  Top
hub  Top.Mid = tcp://127.0.0.1:7816     ; declaration and map entry in one line
hub  Top.Mid.Leaf
at   Remote.Peer = pipe://P2PmsgRemote  ; an address this process does NOT own

link Top     -> Top.Mid                 ; the arrow is not decoration:
link Top.Mid -> Top.Mid.Leaf            ;   left listens, right dials
```

```cpp
p2pf::Topology topo;
p2pf::Topology::parse ( text, topo, &badLine );   // no side effects at all

topo.createHubs ( net );                          // hubs exist, nothing armed
topo.hub ( L"Top.Mid.Leaf" )->onTopic ( L"relay", handler );   // <- the point
topo.arm ( net );                                 // map pushed, edges armed
```

**Two phases, not one call, and that is the whole design.** Handlers must be
registered before anything is armed — the registries are written on the client
thread and read on the pump thread. A single `applyTopology()` would create
hubs and arm edges with no window between, so the convenience would cost the
one ordering rule this layer cannot bend.

An edge's endpoint is chosen most-specific-first: the `link` line's own
endpoint, else the map entry for the **listener**, else empty — an in-process
Dmx link derived from the pair. Same precedence the facade itself uses.

**It does not parse endpoints**, deliberately. The grammar lives inside the DLL
so there is exactly one parser; the `= …` halves are collected verbatim and
handed to `SetEndpointMap`, and only *structure* is checked here — unknown
verbs, a hub declared twice, a `link` naming a hub that was never declared.
When the DLL rejects an endpoint it counts lines in the text *it* was given, so
`parse()` keeps a line-for-line back-map and `arm()` reports the line number in
**your** file.

The shape this was predicted to be good for is the eleven `_TargetCore_UseExamples`
mesh harnesses — all of them "create some hubs, arm some edges, exchange a
message" — and section 19 demonstrates exactly that: a three-hub chain built
from the text above, then a message from `Top` to `Top.Mid.Leaf` that crosses
**both hops with no edge between the ends**.

### `Link` — both ends of an edge, in the order that works

The network object is the only thing in the process holding every live hub, so
it is the only one that can arm both sides of an edge:

```cpp
net.link ( L"Demo", L"Demo.Client" );                 // in-process, no endpoint
net.link ( L"Demo", L"Demo.Client", L"tcp://127.0.0.1:7788" );
```

That deletes the listener-first folklore from in-process call sites (the first
argument is the listener, the second the dialer — the roles are observable, so
the order is part of the contract, not decoration). Everything knowable up
front is checked before anything is manufactured: unknown address
(`P2PF_E_NO_HUB`), self-link, swapped argument, unparseable endpoint, and a
peer either hub already has a connection for (`P2PF_E_CON_DUPLICATE` — a
second `Link` on one pair is an error, not a silent no-op). If the dialer
still fails after the listener armed, the listener is retracted;
`P2PF_E_LINK_PARTIAL` is returned only in the one case where that retraction
itself did not complete, and it names the side left armed rather than leaving
a half-armed edge to be discovered later — `Close()` is whole-hub, so no
public call could clear one.

`Link` is per **edge**, not per hub. A hub with an in-process sibling *and* a
TCP link to another machine still arms the second itself; both hubs must
already exist, so a service that listens and waits still uses `listen`; and
for a chain A–B–C you link A–B and B–C only, because `Send` routes the rest.

### The topology check the kernel never makes

The arming verbs are the one place the facade knows both its own address and
the peer's, so they classify the pair. This matters because the kernel routes
on the dotted address tree but **never checks that a connection joins an
ancestor to a descendant**, and a wrongly-shaped link does not announce
itself — it connects, logs in and looks healthy.

A sibling link — `"App.A"` to `"App.B"` — still carries **direct** traffic both
ways, because `RouteP2PeerMsg` matches the peer's own address before it
consults the tree (`P2PeerHub.cpp:723-725`). What it can never be is a
**transit hop**: both onward rules require an ancestor/descendant relation
(`:727-739`) and the broadcast relay forwards only to children
(`:1235-1237`). So anything addressed *beyond* that peer is silently
undeliverable, and a broadcast arriving over the edge stops there instead of
propagating into the far side's subtree. Two hubs happily exchanging messages
is no evidence that the topology is right.

| relation | result |
|---|---|
| peer is a descendant, or an ancestor (skip levels included) | `S_OK` |
| peer equals this hub's address | `E_INVALIDARG` |
| sibling or unrelated tree | armed, `P2PF_S_UNRELATED_LINK` + one `OnError` |
| peer is a `P2Padomain` pattern (`*`, `Demo.*`, `A\|B`, `Node.#`) | not classified |

`P2PF_S_UNRELATED_LINK` is a **success** code: the link is armed and useful
for talking to that peer. Test it explicitly; `FAILED()` will not see it.

Two more guards, both cheap and both catching failures that are otherwise
unfindable. A `toPeer` containing `:` or `//` is `E_INVALIDARG` — both
parameters are `const wchar_t*`, so a swapped call would otherwise arm a
connection to a peer literally addressed `tcp://:7788`, and a colon in an
address is silently swallowed into the first hop (the `[VNetname:]` qualifier
in the `P2Paddr` grammar was designed and never implemented), after which
every message to that peer dies as `P2Pevent_UNDELIVERABLE` with nothing
reported at arm time. And an empty `toPeer` is `E_INVALIDARG`: it nulls the
connection's identity, so every login would be refused for ever, in silence —
see "An empty `toPeer` is *not* a wildcard" below.

On a hub created with `P2PF_HUB_SECURE` there is a third guard, and it runs
*before* anything is armed: `toPeer` must be someone this hub can authenticate.
For a named peer that means the two files that peer published —
`<peer>.key.pub` and `<peer>.agree.pub` — must be in the security directory,
because the far end may be in another process and copying them across is the
provisioning step no library can perform for you. For a pattern it means the
hub must already be enforcing. Either way the refusal is `P2PF_E_SECURITY`,
with the file or the reason on the diagnostic stream, and nothing is armed —
see "Secure hubs" below. A plain hub, which is what every ABI up to 10 could
create, is unaffected by all of it.

## What `toPeer` means on `Listen`

`Listen(toPeer, endpoint)` reads like a redundancy — a hub can only listen on
itself, and its own address was already fixed at `createHub`. It is not. On
**both** verbs, `toPeer` is the address of the hub on the **other** end. On a
dial it names who you are calling; on a listen it names **who is allowed to
call you**. Your own address is never a parameter, exactly because it is
already known — and the parameter is spelled `toPeer` rather than `peer` so
the call site says which end it means.

The kernel's own naming says it: the factory parameter is `strP2PaddrThat` —
*That*, as opposed to *This* — and `ServiceFactory` stamps it into **two**
members (`P2PeerConWsa.cpp:110-111`, `P2PeerConPipe.cpp:79-80`,
`P2PeerCon232.cpp:102-103`):

| stamped into | what it is | what it does |
|---|---|---|
| `m_oThatP2Paddr` | the connection's own identity (`GetP2Paddress`, `P2PeerCon.cpp:2930-2935`) | the **routing key** — what `Send(dest,…)`, `IsPeerUp(peer)` and the duplicate-peer check match on |
| `m_oP2Padomain`  | a `P2Padomain` **pattern** | the **accept filter** — what an arriving login is checked against (`P2PeerCon.cpp:1877-1885`) |

So a listen here is not a BSD-style anonymous server socket: it arms one
*named*, per-peer connection object. The endpoint (port / pipe name / service
/ COM port) says which local resource to bind; `toPeer` says who may arrive on
it and what the link is called afterwards.

### `toPeer` may be a pattern — that is the wildcard listener

Because the second stamp is a `P2Padomain`, `toPeer` does not have to be a
literal address. `P2Padomain::IsMapped` (`P2Peer.cpp:580-609`) matches
`|`-separated alternatives (`P2Peer.cpp:531`), each glob-matched by
`P2Paddr::IsMapped` (`P2Peer.cpp:450-491`) where `*` matches any run, `?` any
single non-`.` character and `#` a run of digits; a bare `"*"` sets
`m_bWildcard` and short-circuits permissive. What the dialer actually claims
at login is then adopted as the connection's routing key
(`P2PeerCon.cpp:1889-1890`).

```cpp
server.listen ( L"*",      L"tcp://:7788" );   // accept anyone, adopt the name they claim
server.listen ( L"Demo.*", L"tcp://:7788" );   // accept the Demo.* family only
server.listen ( L"Demo.A|Demo.B", L"tcp://:7788" );   // accept either of two
server.listen ( L"Node.#", L"tcp://:7788" );   // Node.1, Node.42, ...
```

Exactly what each form admits — every row is a check in `WildcardListenTest`
(W6), one listener and one dialer per row, in both configurations:

| pattern | claimed name | verdict | why |
|---|---|---|---|
| `*` | `MxAny.At.All` | accepted | special-cased before any matching |
| `MxD.*` | `MxD.Alpha` | accepted | |
| `MxD.*` | `MxD.A.B.C` | accepted | `*` spans dots |
| `MxD.*` | `MxD` | **refused** | the stem alone is not in `MxD.*` |
| `MxD.*` | `MxOther.A` | refused | |
| `MxD.*` | `mxd.Alpha` | **refused** | matching is case-sensitive |
| `MxP.A\|MxP.B` | `MxP.A`, `MxP.B` | accepted | either alternative |
| `MxP.A\|MxP.B` | `MxP.C` | refused | |
| `MxN.#` | `MxN.42` | accepted | `#` = a run of digits |
| `MxN.#` | `MxN.x` | refused | |
| `MxQ.?` | `MxQ.x` | accepted | `?` = exactly one non-`.` character |
| `MxQ.?` | `MxQ.xy` | refused | |
| `MxS.<A,B>` | `MxS.A` | accepted | set expansion, `P2Peer.cpp:611-725` |
| `MxS.<A,B>` | `MxS.C` | refused | |

Three of those are easy to get wrong. **`"*"` is not glob matching at all** — a
token equal to `*` sets `m_bWildcard` (`P2Peer.cpp:534-535`) and `IsMapped`
returns `true` before it looks at the name (`:583-584`). **Matching is
case-sensitive**: the case-folding branch is commented out at `P2Peer.cpp:478`.
And **the scan is driven by the name**, so when the name runs out the pattern
must be exhausted too (`:484-485`) — which is why `MxD` does not match `MxD.*`
though `MxD.` would.

Each alternative is tried first by plain string equality (`P2Peer.cpp:593-594`)
and only then as a glob, so alternatives may themselves be patterns
(`L"Demo.*|Test.#"` is legal) and an ordinary named listen such as
`listen(L"Demo.Client", …)` is just the degenerate case that hits the equality
path. A `,` or `>` outside a `<…>` set is rejected as malformed
(`P2Peer.cpp:707-712`). A pattern is never classified by the topology check —
the far side's real name is unknown until it logs in.

The rest of that test covers the behaviour around the matching:

* one `listen(L"*", …)` served **two** differently-named dialers
  simultaneously; the listener reported `OnPeerUp` for each *claimed* name,
  `IsPeerUp` was true for both, and `sendText` to each adopted name arrived —
  the adopted name is a real route, not just an event label. Dialer→listener
  traffic works the same way.
* `listen(L"Demo.*", …)` is a genuine filter, not a free-for-all:
  `Demo.Alpha` came up, `Other.Beta` was refused and never appeared.
* `listen(L"*", L"pipe://…")` behaves identically — the pattern machinery is
  in the connection object, not in the transport.
* a pattern is still an ordinary peer key, so one hub gets **one** `"*"`
  listener — a second returns `P2PF_E_CON_DUPLICATE`.

This is measured, not inferred from the kernel source: 47 checks, `Debug|x64`
and `Release|x64`, both exit 0, with the whole solution building clean and
`FacadeSmokeTest` still green alongside it. (Both configurations matter here —
`P2Padomain`'s wildcard flag used to be read uninitialised, which the Windows
debug heap's `0xCD` fill made *look* permissive; `P2Peer.cpp:499-507` now
initialises it explicitly, and Release confirms the behaviour is real rather
than a debug-heap artefact.)

Two things this is **not**. It is not authentication: inside the permitted
domain a dialer chooses its own name, so `"*"` means "whoever connects is
whoever they say they are". And the filter is the *only* identity check — a
refused login is dropped with no client-visible event (the peer was never up,
so no `OnPeerDown`), while the facade's retrying dial keeps redialling, so a
misconfigured domain looks exactly like a peer that has not started yet.

**Since ABI 11 there is an answer to the first of those, and it composes rather
than replaces** — see "Secure hubs" below. A hub created with
`P2PF_HUB_SECURE` may hold a pattern listener, and the two checks are
independent: the pattern decides who may *claim* a name, the allow-list decides
whose signature is *accepted*. `Demo.*` on a secure hub listing `Demo.A` and
`Demo.B` admits exactly those two — a stranger who satisfies the pattern is
refused as an unknown peer, because the allow-list lookup is keyed on the
address off the wire and never globs. The one restriction is that a wildcard
may not be a secure hub's *first* arm: with an empty allow-list it has never
turned enforcement on, and would accept anyone. Everything in this section is
about a **plain** hub, which is what it was measured on.

That is **measured**, in `SecureWildcardTest`, and it needed two processes to
be worth anything: both ends of a `Link` are hubs the facade provisions itself,
so a single-process test can only ever produce peers that are already trusted.
The interesting peer is the one that turns up uninvited, and it has to come
from somewhere the facade did not make. See "Two processes, one stranger"
below.

Two sharp edges found while building that test, both of which cost a Release
crash to locate and neither specific to patterns:

* **Closing a hub that still owned live connections corrupted the heap.**
  `RunHub` answers `P2PsigHub_CLOSE` by posting `P2PsigCon_DESTROY` to every
  connection and then *breaking out of the pump loop*
  (`P2PeerHub.cpp:386-396`), so those DESTROYs are never pumped. Teardown
  fell through to `CloseP2PmsgHub`'s sweep, which walked `m_oCListP2PmsgCon`
  with a `POSITION` while each `Destroy()`+`Drop(0)` deleted connections out
  of that same list — its own and, for a listener, the accepted clones it
  owns. `GetNext()` has already advanced to the next node, so when *that*
  node was the one deleted, the next iteration read freed memory: an access
  violation on one run, a fail-fast on the next, at a different connection
  each time. **Fixed on both sides, and both are needed:** the kernel sweep
  now re-derives its position from an index each pass and never holds a
  `POSITION` across `Destroy()`/`Drop()` (`P2Pwin32.cpp`), and `Close()`
  retires the hub's logged-in connections first via `FacadeHub::DrainCons`,
  while the pump is still running to process the request. Regression-tested
  by `WildcardListenTest` W7, which closes the listener first, with the link
  live, eight times over.
* **Two live hubs may not share one address inside a single process.** A
  second `createHub(L"Demo.Alpha")` while the first is still open corrupts the
  kernel's hub registry: later logins are refused for no visible reason, and
  the process dies shortly after. The kernel does not police it, so
  `CreateHub` does — `P2PF_E_HUB_DUPLICATE`, nothing created
  (`FacadeNetwork.cpp`). Closing a hub frees its address again.

The kernel fix alone is enough to stop the *heap corruption* — verified by
reverting `DrainCons` and re-running the reproducer, 4/4 clean where it had
crashed every time before. It is **not** enough for the suite: without the
drain, W7 still fails in Release (a link never comes up, with
`PostP2PmsgCon` reporting *"P2PmsgCon object already posted"* — a connection
left registered by the previous cycle) and aborts in Debug. Orderly
retirement while the pump is alive avoids a class of teardown races that the
sweep fix does not, so the drain stays. It signals only peers that have
actually logged in, never "connection ID 0 == all of them": a dial that has
not connected yet has no completion key and `P2PeerCon::Signal` asserts on
exactly that (`P2PeerCon.cpp:959`, itself marked leftover debugging). It uses
`DESTROY` rather than `CLOSE`, which would merely restart the redial chain,
and is best-effort with a 3 s budget, because `Close()` must never hang.

### An empty `toPeer` is *not* a wildcard

The tempting shortcut — `listen(L"", L"tcp://:7788")` — is now `E_INVALIDARG`,
and that guard is worth its line of code because of what it replaces. The
empty string nulls `m_oThatP2Paddr` as well as the domain, and
`P2PeerCon::OnLogin`'s mandatory identity check
(`P2PeerCon.cpp:1857-1862`, reading `GetP2Paddress()`) throws *"Null local
P2PmsgHub address"* **before** reaching the permissive empty-domain branch. So
the call used to succeed and then never work: every login refused, the dialer
redialling every ~500 ms for ever, nothing reported to the client. Use `L"*"`
when you mean "anyone". A **null** pointer stays `E_POINTER`, distinct from
empty.

W5 in `WildcardListenTest` pins the guard, and it used to be the one case in
the suite that printed a wall of kernel `P2Pevent` dumps — one per refused
login attempt. That spew was the original evidence: it is what pointed at the
domain-pattern mechanism in the first place. If you ever go looking at it
again, read it with one correction in mind — the diagnostic prints
`GetP2PaddrHub()` for **both** the `m_oThisP2Paddr` and the `m_oThatP2Paddr`
field (`P2PeerCon.cpp:3168-3169`), so the con's *That* address always reads
back as the local hub's own address whatever it really holds.

### Asking a hub what it has

A hub records what it armed, because the kernel cannot be asked: every
transport keeps its endpoint in `protected` members with no getter. So the
read side reports the facade's own bookkeeping — what was *requested*, not what
the pump later managed to bind.

```cpp
for ( const auto& c : hub.cons() )
    wprintf ( L"%s  %s  %s%s\n", c.peer.c_str(), c.endpoint.c_str(),
              c.up() ? L"up " : L"", c.unrelated() ? L"UNRELATED" : L"" );

wprintf ( L"%s\n", hub.describe().c_str() );   // the whole hub, one string
```

```
address=Demo
con=Demo.Client	tcp://:7788	listen,up,descendant
con=Other.Peer		up,unrelated
```

Three things it deliberately does:

* **Endpoints round-trip.** What comes back out is the canonical spelling of
  what would go back in, so `describe()` output is a working configuration and
  a `Link`ed listener reads back as the dial it was given with the host dropped
  (`tcp://127.0.0.1:7806` → `tcp://:7806`).
* **It reports the routing relation, not just the wire address.** Per peer:
  child, parent, neither, or an unclassifiable pattern. That is the higher-value
  half — an endpoint string is nice in a log, but "this peer is neither above
  nor below me" is the one-call diagnosis for a mesh that connects, logs in,
  looks healthy and cannot route.
* **It counts peers, not connections it armed.** A wildcard listener arms `"*"`
  and then *learns* real names at login, so a hub can list more peers than it
  armed connections. A learned peer reports an empty endpoint and neither
  `listen` nor `dial` — which is the truth, not a gap.

The flat ABI hands strings back through a caller-sized buffer (`*cch` in =
capacity, out = size required, `NULL` buffer = size query, short buffer =
`ERROR_MORE_DATA` with nothing written). `TargetFacadeFn.hpp` hides all of that
behind `cons()`, `endpointFor()` and `describe()`, and `IP2PHubCom` (dispids
9–13) hides it from every automation tier.

**A failed arm must leave this bookkeeping exactly as it found it**, and for a
while it did not. `ArmRecorded` writes the record *before* posting the
connection — deliberately, so a peer is never visible as up-with-no-endpoint
from an `OnPeerUp` handler — and rolled back by erasing on a failed post. The
one arm that predictably fails is a **duplicate**: a second `Listen`/`Connect`
naming a peer this hub already armed. The first connection is still live and
still routing when the second is refused, and the erase took *its* record with
it, so a working peer read back as one the hub never armed — empty endpoint, no
role, `P2PF_E_UNRESOLVED` from `getEndpoint`. The rollback now restores the
previous entry instead of erasing it, with a check next to the existing
duplicate-peer one in `FacadeSmokeTest`. It was found by the COM automation
probe, which arms a duplicate on purpose and then asks the hub about the peer —
a nice illustration of why a read side is worth having: the first thing it
observed was a bug in the layer underneath it.

### Fixed: closing a hub that owns an unconnected Dmx listener

This used to be a live hazard, and it took **two** fixes — one here and one in
the kernel. It is reachable from ordinary use, because `hub.listen(peer,
nullptr)` arms a Dmx service by construction, and so does `Link` with no
endpoint. Recorded here because the diagnosis is more interesting than the fix.

**Facade half — `DrainCons`.** It retired only peers that had actually logged
in, so an armed-but-never-connected listener was never retired and fell through
to the kernel's teardown sweep, which walks the hub's connection list freeing as
it goes while `Drop` deletes entries underneath the walk. That corrupts the
heap. `DrainCons` now retires two classes: peers that are **up**, and peers this
hub **armed that have never been up** (in `m_mapArmed`, no `m_mapPeers` entry —
`MarkPeer` only ever writes on login or close, so "no entry" is exactly "never
connected"). A peer that *was* up and went down is still excluded: that is the
redialling state, and that exclusion is the original measured restriction, kept.
Signalling a never-connected connection is safe for the reason `CloseCon`
already relied on — `PostP2Pmsg` stamps the pump's IOCP onto a connection on the
way in (`P2Pwin32.cpp:2587-2589`) and nothing ever clears it.

**Kernel half — `~P2PeerConDmx`.** The facade fix stopped the crash but not the
failure. `P2PeerConDmx::Listen` registers `this` in the process-global
`g_oCListP2PeerConDmx` (`P2PeerConDmx.cpp:461`) and **nothing ever removed it** —
not `Drop`, not `OnClose`, not the destructor. So *every* Dmx listener ever
armed left a permanent dangling pointer, `Connect`'s rendezvous scan
dereferenced every entry on every later dial, and `Listen`'s pointer-comparison
guard threw *"Duplicate listen attempted on single connection"* on the next
connection the allocator placed at a recycled address. The list is not exported,
so no facade change could prune it; the deregistration now lives in
`~P2PeerConDmx`, under the critical section the registration already takes. The
destructor and not `Drop`, deliberately — `Drop` also runs on a connection that
is merely closing and may still be `Restart()`ed.

Measured, by section 16 of `FacadeSmokeTest`: close 8 hubs that each own an
armed-but-never-dialled Dmx service, then create an in-process link after each.
Before: **abort on round 3**. With the facade half only: no abort, but **3 of 8**
links completed their login — the rest failed silently. With both: **8 of 8**.

Both halves reach 32-bit as well as 64-bit. That was not free: the 32-bit kernel
would not compile at the time (`P2PeerHub.cpp:74` uses `std::atomic_ref` while
the `Win32` configurations were still on `stdcpp17`), so no x86 `TargetCore`
existed and the 32-bit scripting tier — VB6, 32-bit Office/VBA, WSH, classic ASP,
the widest part of this layer's audience — would have kept the bug. See
`_TargetCore_UseExamplesCom/COM_dependancy.md` under "Two constraints that actually
bite"; `vbs_client.vbs` under `SysWOW64\cscript.exe` is 11/11 on both Win32
configurations against the fixed kernel.

### The gap: a derived Dmx dial did not retry

The four transports did not agree about what a failed dial means, and one corner
of that disagreement was wrong.

| scheme | dial factory | on failure |
|---|---|---|
| `tcp://` | `RetryDialWsa::Make` | redials every 500 ms, forever |
| `pipe://` | `RetryDialPipe::Make` | redials every 500 ms, forever |
| `dmx://` | `P2PeerConDmx::ClientFactory` | **one shot, then gone** |
| `serial://` | `P2PeerCon232::ClientFactory` | **one shot, then gone** |

The retry is not a kernel feature — it is two ~19-line facade subclasses
(`src\FacadeHub.cpp:87-124`) that set `m_uAutoRestart = 500` and override
`HasDroppedOut` to return `true`. Dmx and serial were left raw deliberately: a
missing in-process service or a missing COM port is a *configuration* fault, and
configuration faults should fail fast rather than spin. That reasoning holds for
an endpoint the caller typed. It does not hold for the endpoint the facade
*derives*, and the fix below is exactly as wide as that distinction.

`connect(peer, nullptr)` resolves through `FacadeNetwork::ResolveDial`, which
refuses unless a hub of that address is live in this process **and** has already
recorded a listener expecting us. So "the peer does not exist" is caught at the
call site with `P2PF_E_UNRESOLVED`, and the only failure left is a timing one —
which is exactly the failure the one-shot dial handles worst:

* **The record is written before the connection is armed.** `ArmRecorded` stores
  the endpoint and *then* posts the connection, on purpose (§ *Asking a hub what
  it has*). The arming itself happens later, on the listening hub's pump thread.
  So `ResolveDial` can hand back a perfectly good service name for a service that
  does not exist yet.
* **The rendezvous window can be ~0 ms.** `P2PeerConDmx::Connect` spins until
  `uTimeout = _time64(0)*1000 + 100` (`P2PeerConDmx.cpp:682`). `_time64` has
  one-second granularity, so the real budget depends on where the call lands
  relative to a second boundary: anywhere from ~1000 ms down to almost nothing.
* **The loss is completely silent.** `connect` returned `S_OK` long before the
  throw. The peer never came up, so no `OnPeerDown` fires. Nothing reaches the
  client at all — you get a peer that simply never appears. This is the same
  failure mode section 16 had to detect indirectly, by asserting that links
  actually complete rather than trusting `S_OK`.

`Link` masks it in the common case by arming the listener first, which is why
this is a gap and not a defect list-topper. What remains exposed is a caller who
arms the two ends separately — `hubA.listen(b, nullptr)` in one statement and
`hubB.connect(a, nullptr)` in another, or from another thread — which is
ordinary code, and which `Link`'s own doc comment tells people they may write.

**Why it could not be fixed before.** The retry loop leans on `Connect`'s
rendezvous scan (`P2PeerConDmx.cpp:688-696`), which walks
`g_oCListP2PeerConDmx` and dereferences every entry. Until `~P2PeerConDmx`
learned to deregister (the section above), nothing ever removed entries, so that
scan was a use-after-free on *every* Dmx dial — a redial loop would have been
the maximum-amplification version of that bug.

**The mechanism, and the one that does not work.** Both earlier plans specified
"catch the throw inside `Connect` and call `Restart`". That is infeasible:
`P2PeerCon::Restart` hard-requires close-handler pump state and throws if called
from the startup context `Connect` runs in (`P2PeerCon.cpp:2267-2272`). The
working mechanism is the wrapper the facade already proves out for tcp and pipe
— let the kernel's own chain run: throw → caught at the pump
(`Msgcorewin32.cpp:3054-3068`) → `Drop()` → posts `P2P_Close` → `On_ConClose` →
`Restart(m_uAutoRestart)`.

**And why it must be bounded.** A failed Dmx attempt is not free the way a
failed TCP connect is: the spin blocks the *dialing* hub's pump thread for its
whole 0–1000 ms. If the listener never arms **at all** — its `Listen` threw on
its own pump, which is exactly what the section above was about — an unbounded
redial would leave that pump stalled more than half the time, permanently, with
no diagnostic. So the derived dial gets a *budget*, not an infinite loop.

### Fixed: the derived Dmx dial now retries, on a budget, and says when it stops

`FacadeEndpoint` carries one new field, `bDerived`, set by
`FacadeHub::ResolveEndpoint` and by nothing else. `MakeCon` splits the dmx dial
arm on it:

```cpp
case p2pfDmx:
  if ( bListen )
    return (P2PeerCon*)P2PeerConDmx::ServiceFactory ( peer, rEp.csName );
  return rEp.bDerived
       ? (P2PeerCon*)RetryDialDmx::Make          ( peer, rEp.csName )
       : (P2PeerCon*)P2PeerConDmx::ClientFactory ( peer, rEp.csName );
```

So the policy change is exactly as wide as the problem: **an endpoint the caller
typed still fails fast.** `dmx://SomeService` and `serial://COM5` are unchanged,
because for those "it is not there" really is a configuration fault and retrying
cannot conjure it. That is also why there is no `RetryDial232` — no endpoint is
ever *derived* as serial, so the class would have no caller.

`RetryDialDmx` is the existing wrapper pattern plus a rendezvous budget of **8
attempts**. The budget is spent in the override, before delegating, because the
base call throws out of the frame on a miss and never returns:

```cpp
virtual bool Connect ( )
{
    if ( --m_nTriesLeft <= 0 )
      m_uAutoRestart = 0;              // this attempt is the last one

    bool bDone = P2PeerConDmx::Connect ( );

    m_nTriesLeft   = kRedialDmxTries;  // rendezvous made: budget refreshed
    m_uAutoRestart = kRedialMillisecs;
    return bDone;
}
```

Refreshing on success is what keeps this a *rendezvous* budget rather than a
lifetime one: a link that comes up, runs for a week and then drops gets a full
8 attempts to re-establish, which is the reconnect behaviour tcp and pipe have.
Eight × 500 ms of pacing is far past any plausible pump lag — the thing being
waited for is one `P2P_Listen` dequeue on a sibling thread — and it bounds the
worst case at roughly 4 s of pacing plus up to 8 s of spin.

**Giving up is reported.** Retrying eight times and then going quiet would only
have converted "silently lost once" into "silently lost eight times", so
`FacadeHub::On_ConClose` reads the exhausted flag off the connection *before*
delegating (the base handler may retire the object) and raises one `OnError`:

```
TargetFacade: gave up dialing 'Demo.Server' after 8 attempts at
'dmx://P2PF|Demo.Server|Demo.Client'. The in-process listener never armed, or
the hub that owned it has closed.
```

That is the whole point of the change. The retry closes the race; the message
closes the silence, which was the worse half.

**What section 17 measures, and what it does not.** Two things came out of
instrumenting this that are worth more than the fix:

* **The race is narrower than the design assumed.** Section 17 arms a derived
  link a millisecond before a second boundary, which shrinks the kernel's spin
  to almost nothing — and the listener still wins *every* time. Traced,
  `RetryDialDmx::Connect` ran exactly once per link in every round. The window
  is real (it is why one-shot was the wrong default) but it is narrower than
  anything this suite can force through the public API: the listener's
  `P2P_Listen` is posted before the dialer's `P2P_Startup` and both are
  microsecond-scale. So the retry is asserted by construction, not measured.
* **Closing the far hub does not make the near one redial.** The natural test
  for give-up — link two hubs, close the listener, watch the dialer run out of
  attempts — does not work, because the dialer is never told. Measured: closing
  hub `Gv` produced `On_ConClose` on `Gv` and nothing at all on `Gv.Node`. The
  kernel wakes a paired Dmx connection by completing its pending recv
  (`P2PeerConDmx::Drop`), and a hub that has already retired its connections
  has none pending. That is a real observation about in-process teardown, it is
  pre-existing, and it is not what this change was about — but it does mean the
  give-up report is **unreached by the suite**. It is eight lines of
  correct-by-inspection code guarding a state (`Listen` threw on the far pump)
  that the public API cannot construct on demand now that the section above is
  fixed.

So section 17 pins the two states that *are* reachable: a derived link armed in
the narrowest window the API allows still comes up (six rounds), and a **typed**
`dmx://` dial armed before its listener still stays down — dialer first,
listener 1.5 s later, no `onPeerUp` in four seconds. The second is the more
valuable of the two: it is the check that the new retry did not leak into the
path whose contract is to fail fast.

### Fixed: the login accept filter, which three transports had and the fourth did not

`toPeer` on a listen is **two stamps** on the connection, not one:

| stamp | what it is |
|---|---|
| `m_oThatP2Paddr` | the peer address the connection routes to |
| `m_oP2Padomain` | the **accept filter** — what an arriving login's *claimed*
address is checked against (`P2PeerCon.cpp:1876-1886`) |

The second is the only built-in place a topology error is caught **at login**
rather than diagnosed later from a message that quietly failed to route, and it
is why `toPeer` may be a pattern at all: `Demo.*` is a domain, so a wildcard
listener adopts whatever matching name the dialer claims.

It was not applied uniformly. `P2PeerCon`'s `(addr, io)` constructor sets both
stamps, and `P2PeerConWsa`/`Pipe`/`232`'s factories set the domain again
explicitly — but **`P2PeerConDmx::ServiceFactory` and `ClientFactory` build
from the default constructor and assign only the address**, so the domain stayed
null, and the kernel's guard reads a null domain as "no restriction" (its own
comment at `P2PeerCon.cpp:1871` says so).

So the same `listen(peer, …)` call enforced the peer's claimed identity over
three transports and skipped it on the fourth — and the fourth is what an
**omitted endpoint resolves to**, which is the path this README recommends for
zero configuration.

Measured, section 20: a hub addressed `Flt.Wrong` dialled a `dmx://` service
armed for `Flt.Right` and **logged in**; the identical shape over `tcp://` was
refused with `Login P2Paddr[Flt2.Wrong] is not within domain [Flt2.Right]`.
The Dmx rendezvous matches on the service *name* alone — peer address and
owning hub ignored — which is what makes a wrong claim reachable in the first
place.

The list is not exported and the member is `protected`, so this cannot be fixed
from outside a subclass. The facade therefore builds its own Dmx connections
(`FacadeConDmx`) instead of calling the kernel factories: field for field the
same, including the `P2PeerioDmx` that only those factories ever supplied, plus
the one stamp they omit. Zero kernel changes. Both halves of section 20 now
produce the same kernel error, and a `dmx://` wildcard listener still admits a
matching claim.

## Past the messaging slice (ABI 6)

Everything above this line is **one** subsystem of TargetCore: a hub, its
connections, and traffic between them. `missing.md` is the audit of what that
leaves out — roughly nine other subsystems — and `missing_progress.md` is the
record of closing the cheapest six of its recommendations. Ten methods, all
scalars and strings, so nothing here bends the rules the rest of the header is
built on:

```cpp
hub.disconnect ( L"Demo.Client" );                 // drop ONE peer, keep the hub
hub.setTimer   ( 1000, kSweepKey );                // -> onTimer, ON the pump thread
hub.post       ( kWorkKey, pJob );                 // -> onPost,  ON the pump thread
hub.ping       ( L"Demo.Client", 5000, &ms );      // a real round trip, in ms
hub.setConTrace( L"Demo.Client", true );           // per-connection P2Peerio knobs
hub.onEvent    ( [](unsigned code, auto peer, auto what){ ... } );   // a CODE, not prose
hub.onMessageEx( [](const p2pf::Message&){ return S_FALSE; } );      // "not mine"
void *native = hub.native();                       // the escape hatch
```

Five things about it are worth knowing before you use it.

**The new callbacks are on a second interface.** `IP2PHubEvents` is implemented
by the *client*, so appending to it would make the DLL call a vtable slot the
client's object does not have. `IP2PHubEvents2` derives from it and is handed
over with `SetExtEvents`; a hub without one behaves exactly as it did in ABI 5.
Once one is registered, `OnMessage`/`OnError` are delivered to
`OnMessageEx`/`OnEvent` **instead**, never to both. `TargetFacadeFn.hpp` does
the registration for you and falls back to the plain registries, so
`onTopic`/`onError` code needs no change.

**Three of them are marshalled through a message, not a lock.** `setTimer`,
`post` and `ping` are called on a client thread and their work happens on the
pump; each posts this hub one message under a private `P2PF$` topic, which is
picked out of the receive path before the client sink is reached. That is why
the work lands in FIFO order with the hub's own traffic, and why `Send` and
`Broadcast` now reject the `P2PF$` prefix the way they always rejected
`P2Pmsg` — otherwise a client could hand another hub's `onPost` handler a
pointer of its choosing.

**Two of them wait, and refuse the pump thread.** `disconnect` waits for the
connection to leave the hub's list and `ping` waits for an answer; in both cases
the thread that would satisfy the wait *is* the pump, so calling either from a
callback returns `P2PF_E_PUMP_THREAD` instead of deadlocking.

**Three boundaries are promises rather than warnings.** They shipped as
recorded limitations and are now behaviour, pinned by section 22 of
`FacadeSmokeTest`:

* **A timer never fires early.** The kernel builds its deadline from
  `_time64(0)*1000` and polls it against the same one-second clock, and the two
  truncations cancel only while the pump's next wake-up is the timer's own —
  so on a hub with traffic a 200 ms timer armed just before a second boundary
  fired in 5 ms. The facade keeps its own deadline and re-arms rather than
  deliver ahead of it. It can still be up to about a second *late*: when the
  pump notices is the kernel's granularity, and late is what every timer API
  does.
* **`Close()` and an outstanding `ping` are safe together.** The waiter is
  released, answers `P2PF_E_TIMEOUT`, and the close does not return until it is
  out of the object it is about to free; a ping *started* after a close has
  begun answers `P2PF_E_CLOSED`. The general rule that no other call may race
  `Close()` is unchanged — this is the one call that had to be safe against it,
  because it is the one that blocks.
* **`S_FALSE` from `onMessageEx` means the same thing everywhere.** A declined
  unicast travels on to the kernel's own not-handled report; a declined
  broadcast cannot, because the base handler is the relay that forwards it to
  child hubs and must run regardless — so the facade sends the same report
  itself, after the relay. Either way the sender hears about it, which brings
  up the fourth thing:

**`P2PF_EVT_ROUTING_ERROR` now fires for the bounce it is named after.** It was
raised from `On_P2PeerError`, i.e. for `P2Pmsg_Error` — and nothing in
TargetCore posts one (the only factory that did is commented out,
`P2PeerMsg.cpp:660-676`). What the kernel actually sends back, from
`RouteP2PeerMsg` for an undeliverable message and from `NotHandled` for one
nobody handled, is a `P2Pmsg_Exception`, which was caught by `P2PeerTarget`'s
own map, printed by the event system, and never mentioned to the client. The
facade now reads it on the way past and reports it with the peer that bounced
it and the kernel's own sentence. Existing sinks that never saw a routing error
will start seeing them; that is the fix, not a side effect.

**Two kernel truths shaped what could be exposed at all.**
`P2PeerHub::WakeupHub` is an `ASSERT(0)` stub (`P2PeerHub.cpp:248-262`), so
there is no `resume` — only `closeIdleCons`, which is `PauseHub` under the name
of what it actually does. And the kernel's `P2PmsgPing` has no responder at all
(`P2PeerTarget::On_MsgPing` raises `P2Pevent_UNKNOWN` and returns
`msgCONTINUE`), so `ping` is a facade-to-facade exchange and a peer that is not
a facade hub simply times out.

`GetNative` is the one to think about twice. It hands back the `P2PeerHub`, and
the moment you cast it you need TargetCore's headers, its lib, MFC and its
threading rules back, with none of this layer's guarantees and none of its
bookkeeping. It is here because the alternative is worse: a client needing one
unexposed kernel feature otherwise has to abandon the facade entirely for that
one feature. It turns "impossible" into "your problem", which is the honest
trade for a layer that does not claim parity.

**Seven of the ten reach COM**, appended as dispids 14–20 under the unchanged
`IP2PHubCom` IID, with `OnEvent` and `OnTimer` appended to the event
dispinterface (ids 5–6). The three that do not are a design statement rather
than an omission: `Post` carries a raw in-process pointer, `GetNative` returns
one, and `OnMessageEx`'s *answer* cannot survive a tier that queues every
callback and replays it on a dispatch thread — by the time a sink could answer,
the facade has long since had to tell the kernel whether the message was
handled. See `com\TargetCom.idl`, and "the COM layer" below.

## The message model (ABI 7)

ABI 6 reached past the messaging slice sideways — the hub, its connections,
its clock. ABI 7 goes at the message itself, which `missing.md` §4.2 audits as
the largest thing still closed. Three methods, again all scalars and strings:

```cpp
hub.sendEx      ( L"Demo.Server", L"req", body, n, /*tag*/ 0x1234 );
hub.broadcastEx ( L"census", body, n, sweep, p2pf::P2PF_PRI_HIGH,
                                             p2pf::P2PF_SEND_NO_BOUNCE );
hub.onTopic ( L"req", [&](const p2pf::Message& m)
              { hub.reply ( m, L"ans", answer, len ); } );   // carries m.tag back
```

**The tag is the point.** It is an opaque 32-bit token that travels with the
message and is handed back to the receiver; the facade never reads it. That
closes what §4.2 named as the sharpest practical consequence of the closed
message model — **no request/response correlation**. Two replies arriving out
of order used to be indistinguishable unless the client put a sequence number
in its own payload and agreed a format for it at both ends. `reply()` addresses
the sender and copies the tag, so neither end spends a byte on saying which
request is being answered.

Proved over a **real TCP connection** in section 23, not the in-process Dmx
edge the rest of the suite uses: all of this rides in `P2PeerMsgPrefix`, inside
the message's data blob, and nothing in the kernel promises that blob is
transmitted field for field. It is.

**The receive half is a method on `IP2PHub`, not a callback.** There is no
`IP2PHubEvents3`. `GetMsgInfo` asks the hub about the message it is
*currently delivering*, from inside `OnMessage`/`OnMessageEx` on the pump
thread; anywhere else it answers `P2PF_E_NO_MESSAGE` rather than a stale value.
The reason is the same one that forced `IP2PHubEvents2` to exist: a sink is
implemented by the *client*, so every new per-message field would need a new
interface and a new opt-in, and would make a client rewrite its delivery
handler to read one number. An ABI 6 client adds one line inside the handler it
already has. `TargetFacadeFn.hpp` asks for you and fills `Message::tag`,
`priority`, `flags` and `destination()`.

**A kernel truth, again.** `P2PF_SEND_NO_BOUNCE` is fire-and-forget: a peer
that declines the message says nothing back, which matters now that §5.4 made
bounces visible and a broadcast on an unpopular topic costs one bounce per
uninterested peer per send. `P2PeerMsg` has a control byte for exactly this
(`P2PeerMsgCtrl_EXCEPTIONS`) and four comments in `P2PeerTarget.cpp` point at
`P2PeerMsg::Exceptions()` as the toggle — **which is declared and implemented
nowhere. The class is `dllimport`, so calling it compiles and then fails to
link, and no code anywhere consults that bit.**
So the facade carries the intent itself in that byte, with the polarity
inverted: a set bit means *suppress*, so zero — every message from a non-facade
peer and every message any ABI 5 or 6 client ever sent — keeps today's
behaviour exactly. It covers a decline; it cannot cover an unroutable address,
whose bounce the kernel posts before any facade code runs.

**Priority** is `P2PeerPri*` passed through, and it is honest about being
nearly nothing: it is a queue position inside one pump, so on a hub that is
keeping up it changes not one thing. `P2PF_PRI_DEFAULT` leaves it untouched,
which is what `Send` does and what `SendEx` reduces to.

**All of it reaches COM** — dispids 21–26, IID unchanged — including the four
read properties, which needed a different mechanism to get there. See "the COM
layer" below.

## Named fields (ABI 8)

The payload is one opaque run of bytes, so anything with **structure** has meant
inventing an encoding for it and agreeing that encoding at both ends — the one
thing this facade otherwise never asks of anybody. Fields are the alternative:

```cpp
p2pf::OutMessage m = net.createMessage();
m.setText ( L"state", L"ok" );
m.set     ( L"load",  &load, sizeof(load) );
hub.replyMsg ( incoming, L"status", m );          // carries the tag back too

// on the far side, inside the handler:
std::wstring state = msg.fieldText ( L"state" );
```

**Beside the payload, not inside it.** Fields ride in their own child of the
message root, so `Data()`/`DataSize()` — what every existing peer reads — is
untouched. Adding fields to a message cannot break a receiver, and that
requirement is what ruled out framing them into the payload the way `Broadcast`
frames its topic.

**The message is a value.** `IP2PMessage` holds no hub, no kernel object and no
lock, so it has no thread affinity, is not consumed by sending, and may be
edited and sent again. That is why `CreateMessage` is on the *network* (it
belongs to no hub) and `SendMsg` is on the *hub* (the hub is what sends), and
why there is no question about what happens when a hub closes.

**Presence is a signal.** A field that is present and empty answers `S_OK` with
size 0; an absent one answers `P2PF_E_NO_FIELD`. Keeping those apart is the
difference between a client that can use presence and one that has to encode it
inside the value.

**Two things the build taught us**, both in `missing_progress.md` §7:

* `#include "P2Pmsg.h"` resolves to `..\Msgcore\P2Pmsg.h`, **not** to
  `..\TargetCore\P2Pmsg(2Msgcore).h` — and in the file that compiles, `class
  P3PmsgNode` is entirely commented out, with `typedef P3PmsgField P3PmsgItem`
  left behind. There is no depth, no child count and no cursor, which is why
  fields are a flat map and why the facade writes its own name index beside
  them.
* `P3PmsgData`'s constructors overload on `LPCSTR`, `LPCWSTR` and `const void*`,
  and the string ones take a length in **characters** where the blob one takes
  a size in **bytes**. A `wchar_t*` silently picks the string overload: 26
  bytes handed over came back as 52. The `(const void*)` cast is load-bearing.

Proved over real TCP in section 25 — binary values with embedded NULs included
— because all of this rides in the kernel's message tree and nothing promised
that tree crosses a wire field for field. It does.

**All of it reaches COM too**: a `noncreatable` `P2PMessage` coclass with
`IP2PMessageCom`, `CreateMessage` on the network, and dispids 27–31 on the hub.

Two worked examples, deliberately the same program written twice —
`examples\HubWatchdog` on the flat C++ ABI and `com\examples\watchdog_client.ps1`
late-bound through `IDispatch`. Both are supervisors that ping their peers on a
heartbeat, run a tagged census, and evict the one that stops answering; reading
them side by side is the shortest way to see what each tier can and cannot do.
See `examples\README.md`.

## Who runs the pump (ABI 9)

A hub's pump is a **thread**. Until now the facade always spawned it, so
callbacks arrived somewhere else and every piece of state they touched needed a
lock or a `post()` round trip. `CreateHubEx` lets a client keep the hub on its
own thread instead:

```cpp
p2pf::Hub ui = net.createHub ( L"App.Ui", p2pf::P2PF_HUB_CALLER_PUMPED );
ui.onTopic ( L"tick", [&](const p2pf::Message& m) { /* YOUR thread */ } );
ui.connect ( L"App", L"tcp://10.0.0.7:7788" );

ui.run ( [&]{ return quitting; } );        // a whole main loop
```

and a GUI host that already owns a loop calls `ui.pumpReady()` from wherever it
handles idle — `Pump(0)` drains what is ready and returns at once, which is the
one property a UI thread needs.

The cost is symmetrical and worth stating: **a caller-pumped hub is only as
live as its loop.** Stop pumping and logins stall, timers do not fire, and
peers eventually see the link go idle. A spawned hub cannot be starved that
way, so it stays the default and `CreateHub` is untouched.

What it buys back is `Disconnect` and `Ping`. On a spawned hub those refuse with
`P2PF_E_PUMP_THREAD` inside a callback, because they wait on the very thread
they would be blocking. Here the waiter *is* the pump, so the wait drives it
instead — with one measured exception, pinned in section 26: pinging the peer
whose message you are currently handling still times out, because re-entering
the pump does not re-enter that connection, which cannot receive again until
the frame being dispatched is done with.

**Deliberately not projected to COM.** That tier copies every callback onto a
dispatch thread precisely so a slow script cannot stall the kernel pump; handing
the same script the pump to run would undo the one thing the design is for, and
an STA that stops pumping stops marshalling too. `GetPending` and `GetPumpInfo`
have nothing to say there either, since every COM hub is a spawned one.

The kernel's own name for this is `P2PeerHub::CreateHub` — *"created and run in
context of calling thread ... client is responsible for pumping"* — and it has
been there all along. Three things about it had to be measured rather than read
(`missing_progress.md` §8.3): `RunP2PmsgPump` is declared and exported by
nothing, `GetP2PmsgPumpHANDLE` is NULL for a hub's own pump so there is no
handle to wait on, and `CloseHub` does nothing when called from the thread the
hub runs on — which is the only thread allowed to close one of these.

## The kernel narrates (ABI 10)

TargetCore has been describing everything it does — every refused login,
dropped connection, undeliverable message — into a notification slot that no
facade client could reach. `OnError` handed over one sentence the header tells
you not to parse; `OnEvent` added a code for the four conditions the *facade*
raises. The kernel's own half went nowhere at all.

```cpp
net.onDiag ( [](const p2pf::DiagEvent& d) {
    wprintf ( L"[%s tid=%u] %s: %s\n", d.className().c_str(),
              d.threadId(), d.module(), d.text() );
}, p2pf::P2PF_DIAGM_PROBLEMS );

net.log ( L"Sweep", L"Watchdog: sweep 3 starting" );   // the same stream
```

which in `examples\HubWatchdog` prints exactly this — the supervisor's own
lines and the kernel's, in order, each stamped with the thread that raised it:

```
log[EV005 tid=14100] Sweep: Watchdog: sweep 3 starting
log[EVERR tid=16272] P2PeerCon::OnShutdown: Undeliverable message
```

**It is on `IP2PNetwork`, and delivery is synchronous on the raising thread.**
Both of those are the kernel's shape rather than a preference, and the second
one makes this the only callback in the facade that does not arrive on a pump:
there is one static notification slot in the process, called inline as an event
is disposed of, with no hub anywhere in the path to marshal to. So a handler
must be thread-safe, may run on several threads at once, and should format and
queue rather than do work.

Two more consequences worth knowing before using it. **Taking the slot
displaces the kernel's own file logger** (`MsgexceptionLog` registers in the
same one, last writer wins, and nothing can read the previous occupant back to
chain to it). And **the mask is the facade's filter, not the kernel's** — the
slot is handed everything — so a narrow mask saves your handler, not the
kernel. `eventNo` counts every event that reached the slot including the ones
your mask dropped, so a gap in it is exactly what your filter cost you.

The audit pointed at a different API for all this, and finding out why is most
of what this pass was: `CreateP2PeventSink` / `RegP2PeventCmd` /
`PerformP2PeventNotn` are all present and all exported, and **nothing in the
kernel ever notifies that registry** — `PerformP2PeventNotn` is defined twice
and called from nowhere in the build. `missing_progress.md` §10 has the full
account, along with the three smaller surprises underneath it.

**Deliberately not projected to COM**, for the reason ABI 9 was not: that tier
exists to keep a slow script off the kernel's threads, and this contract is
"you are on the kernel's thread". A script that took the process's one logging
slot would also switch off the host application's.

## Secure hubs (ABI 11)

TargetCore has carried a signed login, a per-connection session cypher, an
allow-list, a revocation list and an arming gate for some time, and **none of
it was reachable from here.** A facade hub was created from an address and a
sink; there was no argument through which a key file, an allow-list or a
revocation list could arrive — so every hub this facade has ever made has been
an unprovisioned one, spawned with the kernel's enforcement switches turned
off. That is the library's own documented migration for a tree nobody has
provisioned, and it is still what `CreateHub` gives you.

The opt-in is **one bit, in the flags word `CreateHubEx` already had**:

```cpp
p2pf::Hub a = net.createHub ( L"Demo",        p2pf::P2PF_HUB_SECURE );
p2pf::Hub b = net.createHub ( L"Demo.Client", p2pf::P2PF_HUB_SECURE );
net.link ( L"Demo", L"Demo.Client" );        // and that is the whole of it
```

```vbscript
' the same thing from a script
Set a = net.CreateSecureHub("Demo")
Set b = net.CreateSecureHub("Demo.Client")
net.Link "Demo", "Demo.Client"
```

Note the verb that did *not* change. `Link` is still `Link`, with the same
three arguments — because **authentication is a property of a hub, not of a
link.** Enforcement in TargetCore is hub-wide with no per-connection override,
so "is this link authenticated" was never a question one link could answer: a
hub either demands a signed login from everything that reaches it or from
nothing. Saying it once, when the hub is made and before it can have a
connection at all, is the only place the answer is not retroactive — a hub that
could be secured later is one whose existing links silently changed terms, and
one that could be relaxed later is one whose secure links silently opened.

What the flag arranges, and none of it appears in the header:

| | |
| --- | --- |
| **identity key** | ECDSA P-256, created on the hub's first run and **found** on every run after — a first-run helper that rotated on restart would change a hub's identity behind the operator's back. Its publishable half is written beside it as `<stem>.key.pub`. |
| **agreement key** | ECDH P-256, the separate key others seal *to*, published as `<stem>.agree.pub`. Deliberately not the identity and deliberately unable to be: different container magic, different entropy, so a swapped file fails loudly. |
| **allow-list** | three columns, which the hub adds to as it is linked. This is the half of provisioning that is not mechanical — *who do you trust* — and where the answer comes from depends on the verb (below). |
| **revocation list** | shared by every secure hub of the process. A **position**, not a feature: an all-comments file is the honest "nothing revoked yet", and a configured list that will not load fails **closed**. |
| **enforcement** | authentication required — a signed login with a per-connection session cypher, on any transport. |

### Where the peer's key comes from

`Link` joins two hubs **this network owns, in this process**, so the key
exchange an operator would otherwise do by hand is a memcpy — the network is
the only object that holds both hubs. Both ends must be secure or neither must:
a secure hub demands a login a plain one holds no key to produce, so the mixed
pair is `P2PF_E_SECURITY` with nothing armed, rather than two connections that
could never come up.

`Listen` and `Connect` can name a hub in **another process, on another
machine**, where this process has no way to learn a public point except to be
handed it. So they read the two files that peer published for itself —
`<peerstem>.key.pub` and `<peerstem>.agree.pub`, out of the same directory this
hub published its own into. **Copying those two files across is the
provisioning step**, and it is the one part no library can do for you. If they
are not there the call is refused, with the file named, rather than armed
without authentication.

A **pattern** listener — `"Demo.*"` — names no peer whose key could be looked
up, so it is admitted on a different question: *is this hub already
authenticating?* It is worth being exact about why, because two true statements
weld easily into a false one. An allow-list entry is not a pattern — the kernel
compares identities with an exact compare, so `Demo.*` in that column admits
nobody. That does **not** make a wildcard listener unauthenticatable, and
nothing in the kernel says it does: the allow-list lookup is keyed on the
source address *off the wire*, and the pattern is applied afterwards as an
accept filter on the name the peer claimed. Auth policy lives on the hub, so an
accepted connection inherits it. A hub that requires authentication and lists
A and B may therefore listen on `Demo.*` and will authenticate exactly those
two — the pattern narrows who may *claim* to log in, the allow-list decides
whose signature is *accepted*, and a stranger who satisfies the pattern still
dies as an unknown peer.

What a wildcard may not be is a secure hub's **first** arm. A hub whose only
listener is a pattern has an empty allow-list, has therefore never turned
enforcement on, and would sit there accepting anyone at all — a hub the client
asked to be secure quietly behaving as a plain one, which is the single outcome
the flag exists to rule out. So that is `P2PF_E_SECURITY`, saying to link or to
listen for one named peer first. A plain hub is unaffected: it takes a wildcard
listener with no provisioning at all, exactly as it always has.

### When enforcement goes on

Not at creation, and the reason is the kernel's: it refuses to *start* a hub
that requires authentication and trusts nobody, because an allow-list that
lists nobody refuses everybody (`p2pauth::ArmEmptyAllow`) — and that refusal is
far better at startup than at 3am on the first connection. So a secure hub is
created holding its keys with enforcement off, and turns it on as it takes the
first peer it can authenticate, at which point the kernel's own arming gate is
re-run and the connection is armed only if the hub passes it. A secure hub with
no peers has nothing to enforce and nothing exposed.

```cpp
unsigned int f = a.securityFlags ( );
// P2PF_SEC_REQUIRED | P2PF_SEC_ARMED | P2PF_SEC_CAN_SIGN
// | P2PF_SEC_CAN_OPEN | P2PF_SEC_REVOCATION
a.securityFingerprint ( );   // "4E3A-2E33-D5B5-EAB1-F13A-4175-0597-F01A"
```

`P2PF_SEC_ARMED` is the bit that matters and it is **not** implied by
`P2PF_SEC_REQUIRED`: a hub can require authentication and be unable to perform
it, and that combination refuses every peer rather than authenticating any. A
fresh secure hub reads `CAN_SIGN | CAN_OPEN | REVOCATION` and neither of the
other two, which is the honest description of a hub holding keys it has not yet
been asked to use.

### What it deliberately does not turn on

TargetCore also defaults to requiring an end-to-end **seal** on any body that
will cross an intermediate hub, and an **origin attestation** on anything
arriving down an ancestor link. Both are properties of an *origin and a
destination*; this flag secures a hub and its *edges*. For routed traffic — the
whole point of `Send` routing multi-hop and of `Link` being per edge — the
origin and destination are two hubs that are not linked to each other and so
are not in each other's allow-lists, and requiring either would refuse every
routed message and every broadcast on a secure hub. A silent break dressed as a
protection is worse than the honest scope, so the scope is honest: **every link
of this hub is authenticated and encrypted.** The agreement key is provisioned
anyway, so a deployment that knows its origin/destination pairs can list them
and raise either switch itself through `GetNative`.

Key material lives in a `p2p\` directory beside `TargetFacade.dll`, resolved
from the module's own path so a copy of the tree elsewhere just works and a
stale key from another build is never picked up silently. `SetSecurityDir`
moves it, and only **before** the first secure hub: afterwards a hub holds keys
loaded from the old one, and a call that appeared to relocate them but did not
would be worse than a refusal. Deleting the directory re-provisions from
scratch — and changes every hub's identity.

### Two processes, one stranger

Everything above about a secure hub is provable in one process except the thing
that matters most: **does the wildcard listener actually turn away someone it
does not know?** In-process it cannot be asked. Both ends of a `Link` are hubs
the facade provisions itself, so the only peers such a test can mint are ones
that are already trusted. The interesting peer has to come from somewhere the
facade did not make.

`test/SecureWildcardTest` is that, over a real TCP socket. One executable, three
modes — it spawns itself, because the child needs the identical facade build:

| | |
| --- | --- |
| **S1** | a secure hub's *named* listener admits a peer from **another process** whose published key it holds — the positive case |
| **S2** | ...and its *wildcard* listener **refuses** a stranger that satisfies the pattern but is not in the allow-list — the question |
| **S3** | ...while a **plain** hub's wildcard listener admits that identical stranger — the control |

S3 is what gives S2 its meaning. The rogue is named `SW.Rogue`, so it *satisfies*
`SW.*` — if it is refused, the pattern is not what refused it. S3 then runs the
same name shape, the same transport and the same pattern against a hub that
differs by exactly one flag, and it comes straight up. One difference between
the two runs, and it is `P2PF_HUB_SECURE`.

Three details are what stop this passing for the wrong reason:

* **The rogue's exit code is asserted exactly**, not merely as non-zero. A child
  that failed to build a hub, could not read the key directory, or had its
  `Connect` refused outright would also be non-zero and would prove nothing. The
  run that means something is the one where the socket connected, the login was
  sent, and the peer still never came up.
* **The stranger had a valid identity of its own.** It is a fully provisioned
  secure hub — it published `SW.Rogue.key.pub`, and its own allow-list trusts
  `SW`, so it signed its login perfectly well. The test reads `SW.allow` back off
  the disk afterwards and asserts it names `SW.Trusted` and **not** `SW.Rogue`.
  The single thing the rogue lacked was a line in that file.
* **The listening side's own `OnPeerUp` is the record**, not `IsPeerUp`. The
  parent waits for each child, and a child closes its hub before exiting — so by
  the time `IsPeerUp` could be asked, the peer is gone whatever happened, and the
  first draft of this test got a false failure out of a connection that had
  worked. An event recorded when it fires outlives the connection, which is the
  only shape that distinguishes *never logged in* from *logged in and went away*.

The key exchange is the real one. A secure hub publishes `<stem>.key.pub` and
`<stem>.agree.pub` when it is created, and a secure `Listen`/`Connect` reads the
far end's two files back; across machines an operator copies them, and here both
processes are pointed at one directory. Note the ordering that forces: the
parent cannot listen for `SW.Trusted` until `SW.Trusted` has published, so the
child is run once in `--provision` mode first. That is not a quirk of the test —
that *is* provisioning.

## Design decisions

* **Pure-vtable interfaces + `extern "C"` factory.** No exported C++ classes,
  no STL/MFC types, no exceptions across the DLL boundary — any MSVC toolset
  (or any vtable-capable language) can consume it. `ABI_VERSION` is checked in
  the factory so a stale client fails cleanly instead of skewing vtables.
* **ABI 2 and 3 appended; ABI 4 is the hard cut.** Appending is binary-safe —
  `ListenEx`/`ConnectEx` sat after `Close()`, the read side after them, `Link`
  after `VersionString()`, nothing above them moved, so a v1 client saw an
  unchanged vtable prefix and the factory could accept 1, 2 and 3 alike.
  Removing the eight typed verbs cannot work that way: every slot after the
  arming pair moves, and an old binary would call the wrong method with the
  wrong arguments. So ABI **4** was a floor, not a step: a client built
  against 1–3 gets `P2PF_E_ABI_MISMATCH` at `P2PF_CreateNetwork` instead of a
  vtable skew, and must be rebuilt. That was the right trade only because
  nothing is deployed against the old shape; had there been, the append-only
  mechanic would have been worth its cosmetic tax indefinitely.
  **ABI 5 is append-only again, and deliberately so** — the deployment map's
  three methods sit on the end of `IP2PNetwork`, nothing moved, `IP2PHub` is
  untouched. The factory therefore accepts a **range**, `ABI_VERSION_MIN` (4)
  through `ABI_VERSION` (5), and a v4 binary runs unchanged; it simply cannot
  see the three new methods. The hard cut was a one-off, not a new habit.
  **ABI 6 appends again**, and for the first time to `IP2PHub`: ten methods
  after `Describe`, plus the optional `IP2PHubEvents2`. The factory therefore
  accepts 4 through 6. The one thing that could NOT be done additively was
  putting the new callbacks on `IP2PHubEvents` — that interface is implemented
  by the client, so appending to it would have the DLL call a slot the client's
  object does not have. Hence a second interface the client opts into.
  **ABI 8 appends one to `IP2PNetwork` and five to `IP2PHub`**, plus the first
  new interface since `IP2PHubEvents2` — `IP2PMessage`. That one is safe to add
  for the reason `IP2PHubEvents2` was not: it is implemented by the *facade* and
  handed to the client, never the reverse, so no existing binary has a vtable to
  be short of.
  **ABI 9 appends one to `IP2PNetwork` and three to `IP2PHub`** — `CreateHubEx`,
  and `Pump`/`GetPending`/`GetPumpInfo` — and changes no existing behaviour
  whatever: a client that never calls `CreateHubEx` gets the hub it always got
  and cannot tell this ABI from the last.
  **ABI 10 appends eight to `IP2PNetwork` and none to `IP2PHub`**, plus a third
  sink interface, `IP2PDiagEvents`. That one is client-implemented, which by
  ABI 6's rule means it had to be a *new* interface rather than a wider
  `IP2PHubEvents` — but nothing in that rule says it must be the same object,
  and here it deliberately is not: a log view is not the thing that handles
  your messages.
  **ABI 11 appends one to `IP2PNetwork` and one to `IP2PHub`**, adds no new
  interface at all, and adds one *bit* — `P2PF_HUB_SECURE`, in the flags word
  `CreateHubEx` has taken since ABI 9. That is the whole opt-in: security is a
  property of a hub, so it is settled where a hub is made, and `Link`, `Listen`
  and `Connect` all keep their slots *and* their signatures. The two appended
  methods only make it operable — `SetSecurityDir` (where the keys are) and
  `IP2PHub::GetSecurityInfo` (whether they took). A client that never passes
  the flag gets the hub and the link it always got. The factory accepts 4
  through 11.
  **ABI 7 appends three more to `IP2PHub`** (`SendEx`, `BroadcastEx`,
  `GetMsgInfo`), so the factory accepts 4 through 7. Note where they are *not*:
  there is no `IP2PHubEvents3`. The receive half of ABI 7 is a method on the
  interface the *facade* implements rather than a callback on the one the
  *client* implements — which is the only direction that is genuinely
  append-only, and the only one that does not make every future per-message
  field cost a new interface and a rewritten handler.
  New HRESULTs are still appended (`0x0208`+): `com\test\ComSmokeTest.cpp`
  compares these codes **numerically**, so renumbering one would compile, link
  and mis-assert.
* **HRESULT + flat automation-friendly parameters** on every method. This is
  the "think dual interfaces" decision: an ATL layer can later wrap this 1:1
  (see road map below) without reshaping anything.
* **Events without macros.** The client implements the 4-method
  `IP2PHubEvents` sink (or `HubEventsBase` with empty defaults), or uses the
  header-only `p2pf::Hub::onTopic(...)` `std::function` registry — the same
  pattern a downstream `PeerNode` client uses instead of `BEGIN_P2PeerMsg_MAP`.
  The one wildcard message map lives *inside* the facade's private hub class.
* **Topic = message name** (PeerNode envelope): a topic is dispatched by the
  kernel's own wildcard map machinery; names beginning with `P2Pmsg` are
  kernel-reserved and rejected (`P2PF_E_RESERVED_TOPIC`).
* **Callbacks fire on the hub pump thread**; payload pointers are valid only
  during the callback. Documented on the interface.
* **Dials retry, so arming order never matters — unless you named the
  endpoint yourself.** A raw `ClientFactory` connection dials once, which makes
  `connect` fail if it runs before the far side's `listen` has armed on its own
  pump thread — measured: back-to-back `pipe://` listen/dial fails in Release
  and survives in Debug purely on timing. Facade dials set the kernel's
  `m_uAutoRestart` instead (~500 ms) and override `HasDroppedOut` to return
  `true`, because `P2PeerCon::OnClose` otherwise shows a **modal message box**
  per failed retry — fatal for a library. Failures surface through
  `OnPeerDown`/`OnError` instead. The line is drawn at what the caller
  *supplied*: a typed `dmx://` or `serial://` endpoint that is not there is a
  configuration fault and still fails fast, while an endpoint the facade
  resolved cannot be misconfigured — it was checked against a live sibling hub
  before it was returned — so the only way it can fail is early, and it retries
  on a budget.
* **Broadcast carries its topic in a framed body.** A broadcast must travel
  under the kernel name `P2Pmsg_BCast` (that is what makes the hub relay it
  onward), so the topic cannot be the message name the way it is for a
  unicast. The body is `[magic][topicChars][topic][payload]`, which keeps it
  binary-safe; an unframed body from a non-facade peer is delivered whole.
* **Regular MFC DLL** (`UseOfMfc=Dynamic`, `CWinApp` instance,
  `AFX_MANAGE_STATE` at exported entries) because TargetCore is MFC-based —
  invisible to clients.

## `com\` — the ATL layer (built, tested)

`com\TargetCom.dll` is an in-proc COM server over the facade, for
scripting / VB / .NET clients. It is optional: a C++ client keeps using the
flat header and never loads it. Nothing in `TargetFacade.h` had to change to
make it possible — that constraint is why the header looks the way it does.

* coclass **`P2PNetwork`** (ProgID `TargetCom.P2PNetwork`) implements
  `[dual] IP2PNetworkCom` over `p2pf::IP2PNetwork`; coclass **`P2PHub`**
  (noncreatable — hubs come from `CreateHub`) implements `[dual] IP2PHubCom`.
  The mapping is mechanical: `const wchar_t*`→`BSTR`,
  `void*+size`→`VARIANT`/`SAFEARRAY(VT_UI1)`, `BOOL`→`VARIANT_BOOL`, HRESULTs
  straight through. Two deliberate exceptions: `Broadcast` returns
  `[out,retval] Delivered` because automation clients never see the facade's
  `S_FALSE` "no peer was up", and `Address`/`VersionString`/`MaxPayload` are
  properties. There are no numeric parameters left to map at all — a port, a
  pipe name, a service and a COM port all ride inside the endpoint `BSTR` and
  are range-checked once, inside the facade, as `P2PF_E_ENDPOINT`.
* **`IP2PHubCom` was reissued, not extended.** Collapsing the eight typed
  methods onto `Listen`/`Connect(toPeer, endpoint)` changes the interface's
  shape, and an interface is immutable once released — so it got a **new IID**
  and clean dispids `1`–`8`, rather than a derived `IP2PHubCom2` alongside the
  old one. A second interface is the orthodox answer when something is
  deployed; nothing is, so keeping a shape no caller uses would have doubled
  the surface for ever. Consequence for anyone with an older build installed:
  **unregister it first** (`regsvr32 /u /n /i:user "…\TargetCom.dll"`)
  — the old CLSID/IID/typelib entries do not clean themselves up, and a stale
  `Interface` key pointing at the previous marshalling shape is exactly the
  kind of thing that fails at run time rather than at registration.
* **The read side was appended, not reissued** (dispids `9`–`13`:
  `RelationTo`, `ConCount`, `PeerAt`, `EndpointFor`, `Description`). `IP2PHub`
  has had a read side since ABI 3; `IP2PHubCom` exposed none of it, so an
  automation client could neither read the arming result nor ask afterwards.
  That mattered because `Listen`/`Connect` can return
  **`P2PF_S_UNRELATED_LINK`** — a *success* code — and automation discards
  success codes: `ITypeInfo::Invoke` normalises them to `S_OK`, and the CLR
  marshaller has nowhere to put one on a `void` method. Only a C++ vtable
  caller ever saw it. Now every tier can ask instead, at any time rather than
  only in the instant of the call:
  `RelationTo(peer) And p2pfRelUnrelated`, or one `Description` string straight
  into a log. Appending after `Close` leaves `1`–`8` at their dispids and their
  vtable slots, so **the IID does not change** and no existing call site moves;
  the four consumer trees needed nothing. The projection is reshaped rather
  than transliterated — the flat `GetCon` packs five parameters and a
  caller-sized buffer protocol, and late-bound `[out] BSTR*` parameters are
  exactly where scripting hosts get fragile, so these are one in, one retval
  and the buffer dance lives in `ComHub.cpp`. Same move `Broadcast` made.
  One consequence of adding under an unchanged IID, worth stating plainly:
  a client built for dispids `9`–`13` running against a **stale registered
  server** calls past the end of that server's vtable, and the IID cannot warn
  it. Unregister an old build first — which was already the standing advice.
* **`Link` was appended to `IP2PNetworkCom`** (dispid `4`), by the same additive
  rule and with the same unchanged IID. `IP2PNetwork::Link` has existed since
  ABI 2 and was flat-ABI-only, which left this tier — the one the COM layer
  exists to serve — arming both ends by hand *and* having to know the ordering
  rule that makes hand-arming work. It cannot learn that rule: a `dmx://` or
  `serial://` dial does not retry, so the listening side must go first, and a
  script that gets it backwards sees no error at all, just a link that never
  comes up. One call now does both ends in the right order, and the endpoint can
  be omitted entirely — `[defaultvalue("")]`, so `net.Link "A", "B"` is a legal
  two-argument call from VBScript or PowerShell and the facade derives an
  in-process Dmx name from the address pair. `Link` answers
  `P2PF_S_UNRELATED_LINK` for a sibling pair like every other arming verb, and
  like them the automation tier cannot see it — ask `RelationTo` afterwards.
* **The security pair was appended** (`CreateSecureHub` and `SetSecurityDir`
  at dispids `9`–`10`, and `SecurityInfo` at hub dispid `32`), again
  additively — `CreateHub` keeps dispid `1` and `Link` keeps dispid `4`
  *and* its three-argument signature, so no existing script and no existing
  early-bound client moves. This is the tier that could not have authenticated
  hubs any other way: everything the flag arranges is file paths, key
  containers and 64-byte points, and a script can express none of it. It is a
  **second creation verb rather than an argument on `CreateHub`**, for two
  reasons — an argument would change a signature every early-bound client is
  compiled against, and `secure` as a defaulted argument leaves
  `CreateHub("Demo")` looking like a decision when it is a default. Two verbs,
  and the one with the word in it means it. `SecurityInfo` is on the **hub**,
  because that is what has a posture, and it answers **prose** rather than a
  flags word for the reason `Description` does: there are no `P2PF_SEC_*`
  constants at this tier to mask against, and a script asking whether security
  is on is reading the answer — *`fingerprint=4E3A-… required=1 armed=1
  signs=1 opens=1 seals=0 revocation=1`*. A plain hub answers it too, with
  `(none)` and zeros: asking whether something is secure should not raise.
* **The deployment map was appended too** (dispids `5`–`7`: `SetEndpoint`,
  `SetEndpointMap`, `EndpointFor`), again additively, again with an unchanged
  IID. This is the tier that wanted it most, for a reason peculiar to it: a
  script cannot be recompiled with a new constant, so before this the only way
  to say where a server lived was to edit the script. `SetEndpointMap` takes the
  whole file as one `BSTR` — no arrays, no `[out]` parameters, nothing a
  late-bound host handles badly — and `EndpointFor` answers an **empty string**
  rather than `P2PF_E_UNRESOLVED` for an address with no entry, because from
  automation "is this configured?" is an ordinary question and must not raise.
  The one place this layer does not simply forward the facade's error is the
  bad-line report: the flat ABI returns the offending line in an `[out]`
  parameter, which is exactly what a script host is worst at, so the number goes
  into `Err.Description` instead — *`SetEndpointMap ( 'line 2' ) failed: …`*.
* **`ISupportErrorInfo` on both coclasses**, so a failed call leaves an
  `IErrorInfo` behind and a client reads a sentence instead of a hex code. This
  is the other half of the bargain "one pair of verbs, transport in a string"
  makes: a typed `ListenPipe(peer, name)` could not be spelled wrongly, and
  `Listen(peer, "tpc://:7788")` can — so the failure moved from compile time to
  run time, and `0x80040208` on its own does not say which part the parser
  objected to. The description names the call, echoes the arguments back, and
  spells out what *would* have been accepted:

  ```
  Listen ( 'Com.Client', 'tpc://:7799' ) failed: the endpoint could not be
  parsed, or names a transport this build does not support. Accepted:
  "tcp://:PORT" to listen and "tcp://HOST:PORT" to dial (a listen may not name
  a host -- the kernel always binds every interface -- and a dial must; port
  1-65535, IPv4 only, IPv6 is not supported), "pipe://NAME", "dmx://SERVICE",
  "serial://COM5" (1-255). An empty endpoint asks the facade to resolve an
  in-process link.
  ```

  That reaches every tier through a different door and needs no code in any of
  them: VB/VBScript get it in `Err.Description` (`ITypeInfo::Invoke` copies
  `IErrorInfo` into `EXCEPINFO`), .NET as the `COMException` message, C++ from
  `GetErrorInfo`. Note what it is *not*: a replacement for the return code.
  Branch on the HRESULT; read the description. It covers every facade code, and
  the two worth calling out are the ones a code alone cannot explain — the
  endpoint grammar above, and `E_INVALIDARG` from the peer-slot guard, which
  says **which** argument was wrong. Both `Listen` parameters are strings, so a
  swapped call is otherwise indistinguishable from a bad address.
* **ABI 6 was appended here too** (dispids `14`–`20`: `Disconnect`, `SetTimer`,
  `KillTimer`, `Ping`, `SetConOption`, `GetConOption`, `CloseIdleCons`), plus
  `OnEvent` and `OnTimer` on the event dispinterface (`5`–`6`), by the same
  additive rule and again with an unchanged IID. There was nothing to reshape:
  every parameter was already a `BSTR` or a `LONG`, which is what made these
  the cheap half of the facade's own additions. Three new enums come with them
  — `P2PConOption`, `P2PConMode`, `P2PConState` — so a typed client says
  `p2pfOptMaxRecv` rather than `3`, and a late-bound one has the values
  documented in one place. `Ping`'s timeout is `[defaultvalue(0)]`, so
  `hub.Ping "Peer"` is a legal one-argument call from VBScript.
  What did **not** come across: `Post` and `GetNative` (raw in-process
  pointers), and `OnMessageEx`'s answer — a sink here does not run until the
  dispatch thread replays the event, which is long after the facade needs to
  tell the kernel whether the message was handled. That is the price of the
  queue, and the queue is what keeps a slow client from stalling the pump.
* **ABI 7 came across whole** (dispids `21`–`26`), which is not obvious, because
  its read half is defined by *when* you may ask. `GetMsgInfo` is legal only
  inside a delivery on the pump thread — and at this tier the client's handler
  runs neither on that thread nor at that time. So the facade is asked at
  **enqueue** time, on the pump, inside the delivery, where the rule permits it,
  and the answers travel with the queued copy. `MsgDest`/`MsgPriority`/`MsgTag`/
  `MsgFlags` read that copy and are valid for the duration of an `OnMessage`
  handler; outside one they answer `p2pfNoMessage`. Four properties rather than
  one method with four outs, because `If hub.MsgTag = myRequest Then` is what
  reads as a value in script.
  The gate is a **causality id** (`CoGetCurrentLogicalThreadId`), not a thread
  id. A marshalled sink runs in the *client's* apartment, so its property read
  arrives back as a fresh incoming call on an RPC pool thread that shares
  nothing with the dispatch thread — a thread compare would reject every
  legitimate read and accept none. `SendEx`/`BroadcastEx` needed no such care:
  every argument past the payload is a `LONG`.
* **ABI 8 needed no new thought at all**, which is the first sign ABI 7's
  mechanism was the right shape: the fields are captured at enqueue time beside
  the scalars, gated on `P2PF_MSG_FIELDS` so a message without any costs the
  pump one bit test, and read back through the same causality gate
  (`MsgFieldCount`/`MsgFieldName`/`MsgField`, dispids 29–31). The message object
  itself is a `noncreatable` coclass `P2PMessage` made by
  `P2PNetwork.CreateMessage`, and `CP2PMessageCom` is a thin forward with no
  dispatch thread, no GIT and no queue — everything `CP2PHubCom` needs exists
  because a hub is not a value, and a message is.
* **Events via a connection point** (`_IP2PHubEvents` dispinterface). Facade
  callbacks arrive on the kernel pump thread, which is not a COM apartment, so
  each hub copies every callback into a bounded queue and replays it on its own
  MTA dispatch thread; sinks are held as **Global Interface Table** cookies and
  re-fetched per fire, so COM marshals into the sink's apartment and a slow
  client handler can never stall the pump. The queued copy owns its bytes,
  which also fixes the facade's "payload valid only during the callback" rule.
  Overflow past 4096 pending events is dropped and reported as an `OnError`
  once the queue drains — never silently.

### Building, registering, testing

```
msbuild "TargetFacade(2026).sln" -p:Configuration=Debug -p:Platform=x64   # all five projects
cd com\test && .\run_com_smoke.ps1                 # stage + register per-user + run + unregister
.\run_com_smoke.ps1 -Config Release
```

`run_com_smoke.ps1` registers with `regsvr32 /i:user`, i.e. `HKCU\Software\
Classes` — no elevation, nothing machine-wide. `com\test\ComSmokeTest.cpp` is a
deliberately **single-threaded-apartment** client with a hand-written
`IDispatch` sink and a message pump: 87 checks, Debug|x64 and Release|x64,
covering CoCreate, both hub properties, connection-point advise/unadvise,
TCP loopback, marshalled `OnPeerUp`, unicast text, a 256-byte `SAFEARRAY`
round-trip, broadcast, the error contract (reserved topic, oversize payload,
type mismatch, closed hub) and idempotent `Close`. All green.

Sixteen of those checks are the read side, and they are the ones that would
have caught the sibling mesh: the two arming calls are now asserted to return
`P2PF_S_UNRELATED_LINK` **exactly** (this caller is the one tier that can see
it), a parent/child arm on the same hub is asserted to return `S_OK` for
contrast, and `RelationTo` is asserted to report `p2pfRelUnrelated` for the
sibling pair and `p2pfRelDescendant` for the parent/child one — from both ends
of the edge, with the `listen`/`dial` bit each side armed. Plus `ConCount`,
`PeerAt` past the end and at a negative index, `EndpointFor` round-tripping the
armed endpoint, `P2PF_E_UNRESOLVED` for a peer the hub never heard of, and
`Description`'s exact format.

Two later blocks cover `Link` and the error objects. `Link` is asserted to arm
**both** ends from one call — the peer actually comes up, which is what
separates it from a one-sided `Listen` that happens to return `S_OK` — over a
derived `dmx://` endpoint nobody configured, and to answer `P2PF_E_CON_DUPLICATE`
on a second call for the same pair and `P2PF_E_NO_HUB` for an address no live
hub answers to. The error-object block checks that a mistyped scheme leaves an
`IErrorInfo` whose description echoes the offending string, names the call and
spells out the grammar, and that a swapped-argument call names the **peer slot**
rather than saying "invalid".

The measurement block that ends the suite gained one check rather than a
section: the duplicate-`Listen`-through-`Invoke` control now also asserts
`EXCEPINFO::bstrDescription`. That is the same field a scripting host turns into
`Err.Description`, so the one in-process measurement stands in for every VB, VBA
and VBScript client.

The last block before teardown is not a feature test but a **measurement**, and
this is the only client that can make it: one hub, one process, the same verb
called down both paths microseconds apart.

```
vtable  Listen -> 0x0004020C                                    (P2PF_S_UNRELATED_LINK)
Invoke  Listen -> 0x00000000 (EXCEPINFO::scode 0x00000000)      the code is simply gone
Invoke  Listen (duplicate) -> 0x80020009 (scode 0x80040204)     DISP_E_EXCEPTION: failures DO survive
Invoke  RelationTo -> 0x00000000 (vt=3, lVal=0x0801)            the read side reaches automation
```

Line 3 is the control that gives line 2 its meaning: `ITypeInfo::Invoke`
preserves every failure and discards every success, so the blind spot is the
documented behaviour of automation rather than a fault in this layer. Line 4 is
the answer to it, on the identical call path. That closed the last *inferred*
row in `ErrorFix.md` — all four caller kinds are now measured.

`com\test\script_client.ps1` is the late-bound proof: `New-Object -ComObject
TargetCom.P2PNetwork`, no header or import lib anywhere, driving a Dmx link
between two hubs. It does **not** sink events, and that is a host limitation
rather than a gap — .NET (so PowerShell) can only bind COM events through an
interop assembly for the coclass, which needs `TlbImp`/an early-bound
reference. Early-bound clients (the C++ test, VB6, C# with an interop) attach
to the same connection point and get the events.

That host limitation is precisely why the read side was added, and this script
is the tier it was added for: `Script.Server`/`Script.Client` are siblings, so
both arming calls returned `P2PF_S_UNRELATED_LINK` and the script saw `S_OK`
twice — no return code, and no event either. It now calls `RelationTo`,
`ConCount`, `PeerAt`, `EndpointFor` and prints `Description`, all late-bound
through `IDispatch`, and reads the sibling shape straight off the flags.

It also drives a **secure hub**, which is the part of this ABI a script could
not have reached in any other shape: everything behind `CreateSecureHub` is an
ECDSA identity, its publishable point, an ECDH agreement key, a three-column
allow-list, a revocation list and the kernel's arming gate, and there is no
signature through which a script could hand over any of it. One verb, and the
hub that comes back demands a signed login from every peer it links to —
`Link` is still `Link`, with the same arguments, because the decision was made
where the hub was.

```
ok    a fresh secure hub holds its keys and requires nothing yet
ok    the peer came up, so the SIGNED login completed both ways
ok    ...and the hub requires auth AND can enforce it
      fingerprint=A088-63CA-F099-4E6A-29EF-75ED-3F97-12E9
      required=1 armed=1 signs=1 opens=1 seals=0 revocation=1
ok    each hub has its own identity fingerprint
ok    a secure hub cannot be linked to a plain one (p2pfSecurity)
ok    ...and the message says which verb makes both ends match
ok    a plain hub reads back as holding no identity at all
```

The assertion that carries it is `armed=1` and not `required=1`: a hub can
require authentication and be unable to perform it, and that state refuses
every peer rather than authenticating any — so a secure hub that had quietly
fallen back to a plain one would pass the `IsPeerUp` check above exactly as
this one does. The first line is the other half worth reading: a secure hub
holds its keys from birth and requires nothing until it has a peer to require
it *of*, because an allow-list that lists nobody refuses everybody and the
kernel will not start such a hub at all.

It also exercises the later additions, and each is worth seeing from this tier
specifically. `$net.Link("Script.Link", "Script.Link.Peer")` — two arguments,
the endpoint defaulted away entirely — replaces the ordered `Listen`-then-
`Connect` pair at the top of the same script, which had to carry the ordering
rule as a comment because nothing in the API told it. The failure paths now
read as prose: a mistyped `tpc://` endpoint and a swapped-argument `Connect`
are each asserted to explain themselves in the message, which is what
`Err.Description` would say under `cscript`.

And the **deployment map** matters here more than anywhere. A script cannot be
recompiled with a new constant, so "where the server lives" had to be edited
into the script itself; now `$net.SetEndpointMap($ini)` takes a here-string (in
production, `FileSystemObject.OpenTextFile(...).ReadAll()`) and the two arming
calls that follow name no endpoint at all — yet link up over TCP. A malformed
line comes back as `line 2` in `Err.Description`, which is the whole reason the
map validates when it is set. 27 checks.

Beyond this smoke test, **`_TargetCore_UseExamplesCom`** rebuilds all eleven
`_TargetCore_UseExamples` harnesses on this layer — 15/15 green on both
configurations, including a VBScript client under `cscript`, which is the
least capable client the layer will ever have.

### Out-of-tree consumers — all three migrated

Collapsing the verbs is source-breaking, and three sibling trees were written
against the old spelling. Each needed one line changed per arming call site, and
nothing subtler:

| tree | uses | state |
|---|---|---|
| `_TargetCore_UseExamplesLight` | the flat C++ ABI | **migrated.** `listenDmx(p, s)` → `listen(p, L"dmx://" s)`, `listen(p, 7788)` → `listen(p, L"tcp://:7788")`. 13/13 on Debug\|x64; 11/13 on Release\|x64 |
| `_TargetCore_UseExamplesCom` | `IP2PHubCom` early-bound | **migrated.** Same rewrite in COM spelling, rebuilt against the regenerated `TargetCom_h.h`. 15/15 on Debug\|x64 and Release\|x64, including both script clients; the 32-bit VBScript client re-verified 11/11 on `Debug\|Win32` and `Release\|Win32` |
| `_TargetCore_UseExamplesNet` | `IP2PHubCom` via a hand-written `TargetComInterop.cs` | **migrated.** New IID plus renumbered dispids in the interop, then the same call-site rewrite. 13/13 on Debug\|x64; 12/13 on Release\|x64 |

A stale C++ binary is not a silent hazard — it fails at `P2PF_CreateNetwork`
with `P2PF_E_ABI_MISMATCH`. A stale COM client fails at `QueryInterface`,
because the IID changed with the shape.

The Release failures in the Light and .NET trees are the **same undiagnosed
teardown access violation**, raised after the harness has printed its verdict,
in the two-process TCP clients. It **predates ABI 4** — measured against
unmodified pre-migration sources built on the pre-migration facade — and it does
not reproduce in the C++ COM tree, whose teardown runs through `CoUninitialize`
and the GIT rather than straight out of `main`. It is worth a look on its own.

Three things the migrations turned up that are worth keeping:

* **The COM layer lost its own range check and lost nothing.** `Port()`, the
  private helper that validated a `LONG` port before narrowing it, is gone with
  the numeric parameters. A bad port is now `P2PF_E_ENDPOINT` out of the one
  parser instead of `E_INVALIDARG` out of a second, divergent rule.
* **The type library can no longer document a transport.** Per-transport
  helpstrings ("Dmx dials do not retry") had nowhere to live once four
  transports shared two methods. That guidance now lives here and in the harness
  headers — a real, if small, cost to anyone reading the interface through an
  object browser.
* **`P2PF_S_UNRELATED_LINK` did not reach a .NET client at all**, and it was the
  one worth acting on. A `void` interop signature — which is what `TlbImp`
  and "Add Reference" generate — discards a *successful* HRESULT: there is no
  exception, because the call succeeded, and no return value, because the
  marshaller consumed it. C# sees `S_OK` where both C++ trees print
  `P2PF_S_UNRELATED_LINK`. Measured across four caller kinds, only the C++
  vtable one could see it; VB, VBScript and C# could not, and a script host
  could not even sink the compensating `OnError`. **Fixed by appending the read
  side** (above) rather than by reshaping the arming pair: the code is a
  *success* code, so it was never going to survive the HRESULT, but the
  condition it reports is still true a second later and can simply be asked
  about. `_TargetCore_UseExamplesNet\TwoConTest` now prints `RelationTo` and
  `Description` next to the `S_OK` its arming calls still report — the two lines
  side by side are the whole lesson. The general rule, which `Broadcast` already
  follows: if an automation-facing method has something to say **on success**, it
  must say it in an `[out, retval]`, a readable property, or an event — never in
  the HRESULT.

## Dependencies

There is no package manager here — no vcpkg, no conan, no CMake. It is MSBuild
plus two prebuilt sibling import libs, and the interesting property of the list
below is how **little** of it a client inherits.

### The DLL — `TargetFacade(2026).vcxproj`

| dependency | kind | where it comes from |
|---|---|---|
| **TargetCore** | sibling MSCS project, `TargetCore.lib` | headers `..\TargetCore`, libs `..\lib\$(Platform)\$(Configuration)` |
| **Msgcore** | sibling MSCS project, `Msgcore.lib` | headers `..\Msgcore`, same lib dir |
| **MFC** (dynamic) | `UseOfMfc=Dynamic` | `afx.h`, `afxwin.h`, `afxext.h`, `afxmt.h`, `afxtempl.h` |
| **Winsock 2 / MSWSock** | `ws2_32.lib`, `MsWsock.lib` | `WinSock2.h`, `mswsock.h`, `ws2tcpip.h` |
| **COM support lib** | `comsuppwd.lib` (Debug) / `comsuppw.lib` (Release) | `comutil.h` — `_bstr_t`, `_variant_t` |
| **Windows SDK 10** + CRT | `WindowsTargetPlatformVersion=10.0` | `windows.h`, `SDKDDKVer.h`, `stdlib.h`, `stdio.h`, `tchar.h` |
| **C++17 standard library** | `LanguageStandard=stdcpp17` | `<map> <list> <string> <utility> <vector>` |

Everything from the two kernel trees enters through exactly one header —
`src/stdafx.h:39-48` — which names ten:

```
P2Pwin32.h  P2PeerHub.h  P2PeerConWsa.h  P2PeerConPipe.h  P2PeerConDmx.h
P2PeerCon232.h  P2Peerio.h  P2PeerioDmx.h  P2PeerMsg.h        <- TargetCore
Msgexception.h                                                <- Msgcore
```

Nine of those come from **TargetCore** and one from **Msgcore**, but that split
describes the `#include` lines, not the dependency. Both trees are on the header
path (`AdditionalIncludeDirectories = include;src;..\Msgcore;..\TargetCore`), a
quoted include falls through to whichever tree has the file, and the TargetCore
headers lean on Msgcore heavily. The real transitive closure is **15 headers
from TargetCore and 13 from Msgcore**:

| tree | headers reached |
|---|---|
| **TargetCore** | the nine above, plus `P2Peer.h`, `P2PeerCon.h`, `P2PeerTarget.h`, `P2PeerExplorer.h`, `TargetCore.h`, `P2PmsgMaps.h` |
| **Msgcore** | `Msgexception.h`, `Msgcore.h`, `P2Pmsg.h`, `P2PmsgBSTR.h`, `P2PmsgMgr.h`, `P2PmsgVBLock.h`, `MsgAttr.h`, `MsgCollectors.h`, `MsgCurs.h`, `MsgDesc.h`, `MsgList.h`, `MsgStck.h`, `MsgVect.h` |

Two of those names are worth watching: `P2Pmsg.h` and `P2PmsgBSTR.h` resolve
into **Msgcore**, even though TargetCore holds files called
`P2Pmsg(2Msgcore).h` and `P2PmsgBSTR(2Msgcore).h`. The parenthesised copies are
not reachable by their `#include` spelling, so they are never what gets
compiled here. Only `comutil.h` and `wtypes.h` fall through to the SDK.

Toolset is **v145** (VS 2026), Unicode, `/MD(d)`, four configurations:
Debug|Release × x64|Win32.

### What a client depends on — deliberately almost nothing

The public headers (`include/TargetFacade.h`, `TargetFacadeFn.hpp`,
`TargetFacadeTopology.hpp`) include `<windows.h>` and the C++17 standard library
and **nothing else** — no MFC, no `afx*`, no TargetCore, no WinSock. A client
links `TargetFacade.lib` and adds `include\` to its header path. That is the
whole contract, and it is enforced rather than asserted: `FacadeSmokeTest`,
`WildcardListenTest` and `HubWatchdog` are all built **without** `UseOfMfc`, so
the solution stops compiling the moment the facade leaks one of its internals.

| project | links | includes |
|---|---|---|
| `test\FacadeSmokeTest` | `TargetFacade.lib` | `..\..\include` |
| `test\WildcardListenTest` | `TargetFacade.lib` | `..\..\include` |
| `examples\HubWatchdog` | `TargetFacade.lib` | `..\..\include` |

### The COM layer — `com\TargetCom(2026).vcxproj`

| dependency | kind |
|---|---|
| **TargetFacade** | `TargetFacade.lib` from `..\$(Platform)\$(Configuration)` — its only non-system link input |
| **ATL** (static) | `UseOfAtl=Static`; `atlbase.h`, `atlcom.h`, `atlctl.h` |
| **MIDL** | `TargetCom.idl` importing `oaidl.idl` / `ocidl.idl` → generated `TargetCom_h.h` + `TargetCom_i.c` into `$(IntDir)` |
| **COM runtime** | `objbase.h`, `process.h` |

Note what is *absent*: the COM server does **not** use MFC and does **not** see
Msgcore or TargetCore. It is a client of the facade like any other, which is why
it can be built and shipped separately.

`com\test\ComSmokeTest` links `ole32.lib`, `oleaut32.lib`, `uuid.lib` and
compiles the generated `TargetCom_h.h`/`_i.c` — it does **not** link the facade
at all, because a real automation client would not.
`com\examples\watchdog_client.ps1` needs only PowerShell and a registered
`TargetComLib`.

### Build order and staging

`..\lib\<Platform>\<Configuration>\` must already hold `Msgcore.lib` and
`TargetCore.lib` — keyed by configuration as well as platform, because the Debug
and Release lib names are identical. Then: facade first, COM layer second (it
reads the facade's import lib out of `..\x64\Debug` and friends), tests and
examples last. At run time `TargetFacade.dll` must sit next to `TargetCore.dll`
and `Msgcore.dll`.

## Building

Open `TargetFacade(2026).sln` (Debug|x64 / Release|x64), or:

```
msbuild "TargetFacade(2026).vcxproj" -p:Configuration=Debug -p:Platform=x64
```

Links against `..\lib\Msgcore.lib` + `TargetCore.lib` (Release:
`..\lib\x64` first) — same layout as every project in `_TargetCore_UseExamples`.
Deploy `TargetFacade.dll` next to `TargetCore.dll`,
`Msgcore.dll` (e.g. `MSCS\bin\Debug64`).

"Armed" is the facade's word for the state a connection is in between `listen`/`connect` returning `S_OK` and the peer actually being up. It means: **this hub has told the kernel to build a connection object for that peer, and the kernel accepted it.** Nothing more.

Concretely, an arm is the whole path in `FacadeHub::ArmResolved` → `ArmRecorded` → `MakeCon` → `PostCon`:

1. The address guards run (empty peer, swapped argument, self-link).
2. The endpoint is recorded in `m_mapArmed` — *before* the connection is posted, deliberately.
3. `MakeCon` manufactures one kernel connection object from the parsed endpoint (`P2PeerConWsa`, `P2PeerConPipe`, `P2PeerConDmx`, `P2PeerCon232`, or one of the `RetryDial*` wrappers).
4. `PostP2PeerCon` registers it on the hub and queues `P2P_Startup`.

At that point the call returns and the connection is *armed*. What has **not** happened yet:

- For a listener: the kernel has not yet bound the socket / created the pipe / registered the Dmx service. That happens later, on the hub's own **pump thread**, when it dequeues `P2P_Startup`.
- For a dialer: nothing has been dialled. `Connect()` runs on the pump too.
- Nobody has logged in, so no `OnPeerUp` has fired and `isPeerUp` is false.

That gap is why the docs repeat that **`S_OK` means "the dial is armed", not "the peer is up"** — and it is the root of several things in this codebase:

- `P2PF_S_UNRELATED_LINK` is returned *from an arm*, so it describes what the edge will be, not what it is.
- A failure that happens during arming on the pump (a `Listen` that throws there) never reaches the caller — the caller already got `S_OK`. That is why the gap-3 regression had to assert links actually come up rather than trusting the return code.
- The gap-4 race exists precisely in this window: the listener's record is written at step 2 while its service does not exist until the pump reaches step 4, so a sibling can resolve against the record and dial a service that is still a few microseconds away from existing.

The bookkeeping distinguishes three states, and `DrainCons` depends on the distinction:

| state | where it shows |
|---|---|
| **armed, never connected** | in `m_mapArmed`, **no** entry in `m_mapPeers` |
| **up** | `m_mapPeers[peer] == true` |
| **was up, now down** (redialling) | `m_mapPeers[peer] == false` |

`MarkPeer` only ever writes on login or close, which is what makes "no entry at all" mean exactly "this connection has never connected".

Read side: `cons()` / `describe()` report **what was armed** — the facade's own record — not what the pump later managed to bind. A row can therefore say `listen, descendant` with no `up` flag, which is an armed listener nobody has answered. There is deliberately no way to ask the kernel what it actually bound; every transport keeps its endpoint in `protected` members with no getter.

Correct — with one qualification. There is no *dial retry* in the kernel; what the kernel has is a **restart** primitive, and the retry policy is built on top of it in the facade.

What the kernel provides:

- `P2PeerCon::m_uAutoRestart` — a public millisecond value, default 0.
- The default close handler `P2PeerTarget::On_ConClose` calls `Restart(m_uAutoRestart)` **only** when `m_uAutoRestart > 99 && mode == P2PeerCon_CLIENT` (`P2PeerTarget.cpp:2591-2603`).
- `P2PeerCon::Restart` itself hard-requires close-handler pump state and *throws* if called from the startup context where `Connect()` runs (`P2PeerCon.cpp:2267-2272`).

So the mechanism exists, but no kernel factory ever sets `m_uAutoRestart`. Every connection the kernel manufactures on its own — `P2PeerConWsa::ClientFactory`, `P2PeerConPipe`, `P2PeerConDmx::ClientFactory`, `P2PeerCon232::ClientFactory` — leaves it at 0, which means one shot. The retry you see is entirely facade-side: three ~19-line subclasses in `src/FacadeHub.cpp` (`RetryDialWsa`, `RetryDialPipe`, and now `RetryDialDmx`) that set `m_uAutoRestart = 500` in their `Make` and override `HasDroppedOut` to return `true`.

The `HasDroppedOut` override is not optional decoration. Default is `false`, and `P2PeerCon::OnClose` puts the precipitating event in a **modal message box** unless it returns true — one box per failed retry, which would wedge a headless process. That is why a retry loop cannot simply be switched on by setting the timer.

The chain a facade dial actually rides is all kernel code, just kernel code nobody was triggering:

```
Connect() throws
  → caught at the pump          (Msgcorewin32.cpp:3054-3068)
  → Drop()
  → posts P2P_Close             (P2PeerCon.cpp:1144-1152)
  → default On_ConClose
  → Restart(m_uAutoRestart)     (P2PeerTarget.cpp:2591-2603)
```

Two things also worth separating from "retry":

- **The Dmx rendezvous spin** inside `P2PeerConDmx::Connect` (`goto TOP` until `_time64(0)*1000 + 100`) is a *wait*, not a retry — one connection attempt that polls for a listener, then throws. It is what the 0–1000 ms window refers to.
- **The `RetryDialDmx` budget** (8 attempts, then `m_uAutoRestart = 0`) is counted in the facade wrapper, because `m_uAutoRestart` is a period with no notion of a count — the kernel would repeat forever.

One consequence that came out of tracing section 17: a redial is only ever triggered by a close the *dialing* hub observes. Closing the far hub produced `On_ConClose` on that hub and nothing at all on the dialer, so no restart chain started there.
---

## Licence

Distributed under the **Apache License, Version 2.0**. See [LICENSE](LICENSE).

```
Copyright © 2026 Khrustal & Mann
             MELBOURNE, VICTORIA, AUSTRALIA, 3000
```

Some files in this repository are **not** covered by that licence — Microsoft project-template
and wizard-generated files keep their own notices, and the MFC / ATL / Visual C++ runtime /
Windows SDK components this library links against are licensed separately and are not bundled
here. TargetCore and Msgcore are separate repositories under the same licence and are likewise
not bundled. [NOTICE](NOTICE) lists every one of them.
