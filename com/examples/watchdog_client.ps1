# Copyright © 2026 Khrustal & Mann
#              MELBOURNE, VICTORIA, AUSTRALIA, 3000
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing
# permissions and limitations under the License.
#
# watchdog_client.ps1 -- the COM twin of examples\HubWatchdog, in one page.
#
# The same supervisor idea (ping the peers, evict the one that stops
# answering), driven entirely late-bound: no compiler, no header, no import
# lib, no interop assembly. Everything below goes through IDispatch and the
# registered type library.
#
# What this tier gets from facade ABI 6, and what it does NOT:
#
#   Ping                     yes -- and this is the one that changes what a
#                            script can know. Before it, "is that peer alive"
#                            could only be answered by IsPeerUp, which reports
#                            the login state, not whether anything still
#                            answers.
#   Get/SetConOption         yes -- the connection's mode, state, limits, and
#                            the trace switch, none of which had any face here.
#   Disconnect               yes -- and it is the difference between a script
#                            that can drop one peer and one that could only
#                            Close the whole hub.
#   SetTimer / KillTimer     the METHODS reach here; the OnTimer EVENT does
#                            not reach POWERSHELL, because .NET can only bind
#                            COM events through an interop assembly for the
#                            coclass (TlbImp / an early-bound reference). VB6,
#                            C# with an interop, and the C++ ComSmokeTest all
#                            get OnTimer and OnEvent from the same connection
#                            point. So the timer is armed and cancelled here to
#                            show the calls; the heartbeat itself is a loop.
#   Post / GetNative         no, by design: both carry raw in-process pointers,
#                            which no automation tier can hold.
#   OnMessageEx's answer     no: events here are queued and replayed on a
#                            dispatch thread, long after the facade needed the
#                            answer. Automation gets OnMessage as it always did.
#
# And from facade ABI 7:
#
#   SendEx / BroadcastEx     yes -- a correlation tag, a priority and the
#                            fire-and-forget flag, all as plain LONGs, which is
#                            why they were the cheap half to project.
#   MsgTag / MsgDest /       the PROPERTIES reach here and answer during an
#   MsgPriority / MsgFlags   OnMessage event -- which PowerShell cannot receive,
#                            for the same interop-assembly reason OnTimer
#                            cannot. So what this script can show is the other
#                            half of their contract: read OUTSIDE an event they
#                            answer p2pfNoMessage -- though see the check
#                            itself for what PowerShell does with that. The
#                            C++ ComSmokeTest reads them from inside a handler,
#                            in a different apartment from the thread that
#                            raised the event, and gets the tag back.
#
# And from facade ABI 8:
#
#   CreateMessage /          yes, and this is the one that most changes what a
#   SetField / SendMsg       SCRIPT can send. A message carries named fields
#                            beside its payload, so putting three values in one
#                            message no longer means inventing a payload format
#                            and writing the matching parser in whatever
#                            language the far end happens to be.
#   MsgField / MsgFieldName  published, and read the same way MsgTag is -- so
#                            the same event limitation applies here too.
#
#   .\run_com_smoke.ps1 -KeepRegistered      # register the DLL per-user
#   .\..\examples\watchdog_client.ps1
#   regsvr32 /u /n /i:user "..\..\out\x64\Debug\TargetCom.dll"

[CmdletBinding()]
param([ValidateSet('Debug','Release')] [string] $Config = 'Debug')

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$runDir  = Join-Path $here "..\..\out\x64\$Config"

# The kernel DLLs resolve against the PROCESS directory, so run from where
# run_com_smoke.ps1 staged them.
Push-Location $runDir

# --- the enums, by value ----------------------------------------------------
# A late-bound host has no typelib constants, so these are the P2PConOption /
# P2PConMode / P2PFError values spelled out. They are part of the ABI and are
# only ever appended to.
$optTrace     = 1
$optMaxSend   = 2
$optMaxRecv   = 3
$optEncrypted = 4
$optConState  = 5
$optMode      = 6

$modeDial     = 1
$modeListen   = 2

$stateLogin   = 0x0020

# P2PPriority / P2PSendFlag (facade ABI 7)
$priHigh      =  2
$priDefault   = -1        # leave it alone -- identical to plain Send
$sendNoBounce =  1

$script:fails = 0
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok    $what" }
    else     { Write-Host "  FAIL  $what"; $script:fails++ }
}

try {
    $net = New-Object -ComObject TargetCom.P2PNetwork
    Write-Host "        $($net.VersionString)"

    $watch  = $net.CreateHub("ComWatch")
    $node   = $net.CreateHub("ComWatch.Node")
    $zombie = $net.CreateHub("ComWatch.Zombie")

    # Link arms both ends in the order that works -- a dmx:// dial does not
    # retry, so the listener must go first, and a script has no way to learn
    # that by itself.
    $net.Link("ComWatch", "ComWatch.Node")
    $net.Link("ComWatch", "ComWatch.Zombie")

    for ($i = 0; $i -lt 100 -and -not ($watch.IsPeerUp("ComWatch.Node") -and
                                       $watch.IsPeerUp("ComWatch.Zombie")); $i++) {
        Start-Sleep -Milliseconds 50
    }
    Check ($watch.IsPeerUp("ComWatch.Node") -and $watch.IsPeerUp("ComWatch.Zombie")) `
          "both peers logged in"

    # ---- what the links ARE -------------------------------------------------
    # Kernel facts, read straight off the connection object. Before ABI 6 this
    # tier could see the facade's bookkeeping (EndpointFor, Description) and
    # nothing else.
    Write-Host "`n-- the links --"
    for ($i = 0; $i -lt $watch.ConCount; $i++) {
        $peer  = $watch.PeerAt($i)
        $mode  = $watch.GetConOption($peer, $optMode)
        $state = $watch.GetConOption($peer, $optConState)
        $recv  = $watch.GetConOption($peer, $optMaxRecv)
        $enc   = $watch.GetConOption($peer, $optEncrypted)
        $which = if ($mode -eq $modeListen) { "listening" }
                 elseif ($mode -eq $modeDial) { "dialing" } else { "accepted" }
        Write-Host ("        {0,-18} {1}, state 0x{2:X4}, recv<={3}, {4}" -f `
                    $peer, $which, $state, $recv, $(if ($enc) {"encrypted"} else {"clear"}))
    }
    # MODE is the one option answered from the connection this hub ARMED
    # rather than the one carrying the session -- it reports what this hub
    # DID, and must agree with what Description prints (listen / dial).
    Check ($watch.GetConOption("ComWatch.Node", $optMode) -eq $modeListen) `
          "GetConOption reports the listening side's role"
    Check ($node.GetConOption("ComWatch", $optMode) -eq $modeDial) `
          "...and the dialing side's, from the other end of the same edge"

    # BOTH ENDS OF ONE EDGE AGREE, and it took a kernel change to make that
    # true. A named listen leaves TWO connections on the listening hub
    # answering to the same peer address: the service it armed, and the clone
    # the kernel spawned when the peer arrived. The clone is the one that logs
    # in and carries the traffic; the service goes on waiting for the next
    # peer. The kernel's own lookup returns the first of the two -- the
    # service -- so a listening hub used to report an idle object here: no
    # Login bit, and a trace switched on that went nowhere. The facade now
    # walks the hub's connections and picks the one carrying the session.
    $sDial   = $node.GetConOption("ComWatch", $optConState)
    $sListen = $watch.GetConOption("ComWatch.Node", $optConState)
    Write-Host ("        state: dialing end 0x{0:X4}, listening end 0x{1:X4}" -f $sDial, $sListen)
    Check (($sDial -band $stateLogin) -ne 0)   "the DIALING end carries the Login bit"
    Check (($sListen -band $stateLogin) -ne 0) "...and so does the LISTENING end"

    # A knob no tier could write before.
    $watch.SetConOption("ComWatch.Node", $optTrace, 1)
    Check ($watch.GetConOption("ComWatch.Node", $optTrace) -eq 1) "SetConOption(Trace) round-trips"
    $watch.SetConOption("ComWatch.Node", $optTrace, 0)

    # ---- the timer methods --------------------------------------------------
    # Armed and cancelled rather than awaited: see the note at the top about
    # events and PowerShell.
    $timerId = $watch.SetTimer(30000, 42)
    Check ($timerId -ne 0) "SetTimer returns the id its OnTimer event will carry"
    $killed = $true
    try   { $watch.KillTimer($timerId) } catch { $killed = $false }
    Check $killed "KillTimer cancels it"

    # ---- the message model --------------------------------------------------
    # A census stamped with the sweep number, ahead of ordinary traffic, and
    # asking for no bounce from peers that do not answer censuses. Everything
    # past the payload is a plain LONG, which is exactly why this half of ABI 7
    # projects and Post/GetNative do not.
    Write-Host "`n-- the message model --"
    Check ($watch.BroadcastEx("census", "?", 1, $priHigh, $sendNoBounce)) `
          "BroadcastEx sends a tagged, high-priority, fire-and-forget census"
    $watch.SendEx("ComWatch.Node", "census", "?", 1, $priDefault, 0)
    Check $true "SendEx unicasts one, tagged, with the priority left alone"

    # The other half of the properties' contract, and a lesson about this tier
    # that only a script can teach.
    #
    # MsgTag outside a message event answers p2pfNoMessage with a sentence
    # attached -- and PowerShell shows neither. ITS COM ADAPTER SWALLOWS A
    # FAILING PROPERTY GET AND HANDS BACK $null: no exception, no
    # Err.Description, nothing to catch. (A failing METHOD does throw with the
    # sentence intact -- see the Disconnect check further down, which prints
    # it. The difference is the adapter's, not the facade's.)
    #
    # So a script that reads these must test for $null. A VB6 or C# client
    # gets the refusal and the sentence, as the C++ ComSmokeTest does.
    $tagOutside = $watch.MsgTag
    Check ($null -eq $tagOutside) `
          "MsgTag outside a message event yields nothing (PowerShell eats the refusal)"

    # ---- named fields (facade ABI 8) ----------------------------------------
    # A RECORD, built and sent from script, with no encoding agreed anywhere.
    # This is the piece that most changes what a script can send: before it,
    # putting three values in one message meant inventing a payload format and
    # writing the matching parser in whatever language the far end is in.
    Write-Host "`n-- named fields --"
    $msg = $net.CreateMessage()
    $msg.SetFieldText("state", "ok")
    $msg.SetFieldText("host",  "ComWatch.Node")
    $msg.SetPayload("body")
    Check ($msg.FieldCount -eq 2) "CreateMessage + SetFieldText builds a record"
    Check ($msg.FieldName(0) -eq "state") "...FieldName gives them back in order"

    # GetField answers a byte array. A field set with SetFieldText carries its
    # terminator, so the text is one character shorter than the array is wide.
    $bytes = $msg.GetField("state")
    Check ($bytes.Count -eq 6) "...GetField reads a value back as bytes"

    $watch.SendMsg("ComWatch.Node", "record", $msg, 42, $priDefault, 0)
    Check $true "SendMsg puts a record on the wire"

    # Not consumed by sending: the same object goes again, and can be edited
    # in between. It holds no hub and no kernel object -- it is a value.
    $msg.SetFieldText("state", "still ok")
    $watch.SendMsg("ComWatch.Node", "record", $msg, 43, $priDefault, 0)
    Check ($msg.FieldCount -eq 2) "...and the same object sends again, edited"

    Check ($msg.RemoveField("host")) "RemoveField answers True..."
    Check (-not $msg.RemoveField("host")) "...then False, rather than raising"

    # The refusal a script is most likely to hit, and it DOES explain itself:
    # SetFieldText is a method, and a failing method throws with the sentence
    # intact -- unlike the property gets above.
    $whyField = ""
    try   { $msg.SetFieldText("P2PFmine", "x") }
    catch { $whyField = $_.Exception.Message }
    Check ($whyField -match "reserved|P2Pmsg") "a reserved field name explains itself"
    Write-Host "        $whyField"

    $msg.Clear()
    Check ($msg.FieldCount -eq 0) "Clear empties it for reuse"

    # ---- the heartbeat ------------------------------------------------------
    Write-Host "`n-- heartbeat --"
    $misses = @{}
    $gone   = @()
    for ($sweep = 1; $sweep -le 4; $sweep++) {
        Write-Host "    sweep $sweep"

        if ($sweep -eq 1) {
            # The zombie goes away. Note what does NOT happen: the supervisor's
            # connection stays armed and IsPeerUp can still read True for a
            # moment -- which is exactly the failure a round trip catches and a
            # connection state does not.
            $zombie.Close()
            Write-Host "      (ComWatch.Zombie has gone away)"
        }

        for ($i = 0; $i -lt $watch.ConCount; $i++) {
            $peer = $watch.PeerAt($i)
            if ($gone -contains $peer) { continue }

            $ms = -1
            try   { $ms = $watch.Ping($peer, 600) }
            catch { $ms = -1 }          # P2PF_E_TIMEOUT arrives as an exception

            if ($ms -ge 0) {
                $misses[$peer] = 0
                Write-Host ("      {0,-18} {1} ms" -f $peer, $ms)
            }
            else {
                $misses[$peer] = 1 + [int]$misses[$peer]
                if ($misses[$peer] -gt 2) {
                    Write-Host ("      {0,-18} no answer x{1} -- evicting" -f $peer, $misses[$peer])
                    $watch.Disconnect($peer)      # ONE peer; the hub keeps running
                    $gone += $peer
                }
                else {
                    Write-Host ("      {0,-18} no answer ({1})" -f $peer, $misses[$peer])
                }
            }
        }
    }

    Check ($gone -contains "ComWatch.Zombie") "the silent peer was evicted"
    Check (-not $watch.IsPeerUp("ComWatch.Zombie")) "...and the hub agrees it is down"
    Check ($watch.IsPeerUp("ComWatch.Node")) "...while the healthy peer is untouched"

    # Disconnect of a peer this hub has no connection for is p2pfNoPeer
    # (0x8004020D), and the description says so in words -- which is the whole
    # bargain of an endpoint (and now an option) being a runtime value.
    $said = ""
    try   { $watch.Disconnect("ComWatch.NoSuchThing") }
    catch { $said = $_.Exception.Message }
    Check ($said -match "no connection for that peer") "a bad peer explains itself"
    Write-Host "        $said"

    # Housekeeping this tier could not ask for before. Note its effect on the
    # snapshot below: the surviving link is IDLE by now, so it closes, and
    # Description prints it without `up`. That is CloseIdleCons doing exactly
    # what it says -- ask before the sweep if you want the live picture.
    $watch.CloseIdleCons()
    Start-Sleep -Milliseconds 300
    Write-Host "`n        Description:"
    $watch.Description -split "`n" | ForEach-Object { if ($_) { Write-Host "        $_" } }

    $node.Close()
    $watch.Close()

    Write-Host ""
    if ($script:fails -eq 0) { Write-Host "OK ($script:fails failures)" }
    else                     { Write-Host "FAILED ($script:fails failures)" }
    exit $script:fails
}
finally {
    Pop-Location
}
