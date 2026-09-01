# examples

Worked examples of the facade, one per tier. Each is a complete program, not a
snippet: it builds, it runs, and it exits 0 when it did what its header comment
says it does.

| example | tier | shows |
|---|---|---|
| `HubWatchdog/` | flat C++ ABI (`TargetFacadeFn.hpp`) | every ABI 6, 7 and 8 addition, in the shape they were added for |
| `../com/examples/watchdog_client.ps1` | late-bound COM (IDispatch) | the ABI 6, 7 and 8 surface that reaches automation |

The two are deliberately the **same program** written twice — a supervisor hub
that pings its peers on a heartbeat and evicts the one that stops answering.
Reading them side by side is the shortest way to see what each tier can and
cannot do.

## HubWatchdog — the flat C++ ABI

```
cd examples\HubWatchdog\x64\Debug && HubWatchdog.exe     # exit 0 = as advertised
```

Three hubs in one process: `Watch` (the supervisor), `Watch.Node` (answers) and
`Watch.Zombie` (stops answering, and is dropped for it).

| ABI 6 addition | what it does here |
|---|---|
| `setTimer` / `onTimer` | the heartbeat, on the hub's own thread — no extra thread to race |
| `ping` | the liveness question answered end to end, rather than inferred from "the connection is still open" |
| `post` / `onPost` | the sweep's results, handed back to the pump so the tally has exactly one writer and no lock |
| `disconnect` | evicting one peer with the hub and every other link untouched |
| `getConOption` | what each link actually is: dial or listen, encrypted or not, its receive limit |
| `onEvent` | diagnostics with a code to branch on instead of a sentence to pattern-match |
| `onMessageEx` | declining a topic this hub does not own, so the kernel's own unknown-message reporting still runs |
| `native` | named, printed, and deliberately **not** cast |

and then ABI 7, which is what turned the census from a shout into a conversation:

| ABI 7 addition | what it does here |
|---|---|
| `broadcastEx(tag)` | one census to every peer, stamped with the sweep number |
| `reply` / `Message::tag` | a worker answers the sender carrying that stamp back, so the supervisor can tell sweep 4's answers from sweep 3's late ones — with nothing about it in the payload and no format agreed at either end |
| `P2PF_PRI_HIGH` | the census jumps the queue on a busy hub |
| `P2PF_SEND_NO_BOUNCE` | and asks for silence from peers with no interest in `census`, instead of a bounce per peer per sweep |

Before the tag, correlating a reply to its request meant putting a sequence
number *in the payload* and agreeing a wire format for it on both sides —
which `missing.md` §4.2 named as the sharpest practical consequence of the
kernel's message model being closed. The run prints it:

and then ABI 8, which turned the answer from a sentence into a record:

| ABI 8 addition | what it does here |
|---|---|
| `createMessage` / `setText` / `set` | the worker replies with `state`, `host` and a binary `load` as three named fields — **and no encoding is agreed anywhere** |
| `Message::fieldText` / `field` | the supervisor reads the two it cares about by name, and would not notice if the worker added a fourth |
| `replyMsg` | the same record, addressed back to the sender, carrying the sweep tag |

Before fields, three values in one reply meant inventing a payload format and
writing the matching parser in both programs, kept in step by hand. The run
prints both halves at once:

```
    sweep 3
      Watch.Node        0 ms
      Watch.Zombie   no answer x3 -- evicting
      Watch.Node     answers sweep 3  state=ok load=20
```

**The rule the example exists to demonstrate**: `onTimer` runs on the pump
thread, and `ping` waits for that thread to deliver the answer — so pinging
from inside the timer handler would deadlock the hub for the whole budget. The
facade refuses it (`P2PF_E_PUMP_THREAD`) instead of letting you discover it in
production, and `disconnect` is refused there for the same reason. So the
timer only *wakes* the sweep:

```
onTimer (pump) --SetEvent--> sweep (main thread) --post()--> back on the pump
```

That triangle is the whole design of the example, and it is the shape any real
supervisor written on this ABI will have.

## watchdog_client.ps1 — the same thing, late-bound

```
cd com\test && .\run_com_smoke.ps1 -KeepRegistered
..\examples\watchdog_client.ps1
regsvr32 /u /n /i:user "..\x64\Debug\TargetCom.dll"
```

No compiler, no header, no import lib, no interop assembly — everything through
`IDispatch` and the registered type library. Seven of the ten ABI 6 methods
reach this tier; the three that do not, and why, are listed at the top of the
script and in `com\TargetCom.idl`.

`SendEx` and `BroadcastEx` come across whole — every argument past the payload
is a `LONG`, which is what made them the cheap half of ABI 7 — and the four
`Msg*` properties are published. Reading them needs an `OnMessage` event,
though, so what the script can demonstrate is the *other* half of their
contract: outside one, they refuse.

**ABI 8 is where this tier gains the most.** A script can now build a record and
send it — `CreateMessage`, `SetFieldText`, `SendMsg` — with no encoding agreed
anywhere, which previously meant inventing a payload format and writing the
matching parser in whatever language the far end happened to be. The example
also shows the message being *edited and sent again*, because it is a value and
sending does not consume it.

Three things it shows that the C++ example cannot:

* **Both ends of one edge agree on the connection state** — which took a kernel
  change to make true. A named listen leaves *two* connections on the listening
  hub answering to the same peer address (the armed service, and the clone the
  kernel spawns when the peer arrives); the clone carries the session, and the
  kernel's own lookup returns the service. `MODE` is the one option still
  answered from the armed connection, because it reports what this hub *did*.
* **A failure explains itself in words.** `Disconnect` on an unknown peer
  arrives as an exception whose message names the call, echoes the argument and
  says what would have been accepted — which is the bargain `ISupportErrorInfo`
  exists to make once an option is a runtime value rather than a method name.
* **…except through a property, where PowerShell eats it.** `MsgTag` read
  outside a message event fails with `p2pfNoMessage` and a sentence, and
  PowerShell's COM adapter hands back `$null` with no exception at all. A
  failing *method* throws with the text intact; a failing *property get* does
  not. That is the adapter's behaviour, not the facade's — VB6 and C# see the
  refusal — and a script reading these must test for `$null`.

`OnTimer` and `OnEvent` do reach COM, on the connection point — but not
PowerShell, which can only bind COM events through an interop assembly. The C++
`com\test\ComSmokeTest.cpp` sinks both and asserts them, and it is also where
the `Msg*` properties are read from *inside* a handler — in a different
apartment from the thread that raised the event, which is the case that decides
whether they can work at this tier at all.
