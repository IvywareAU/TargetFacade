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
# script_client.ps1 -- the point of the dual interfaces, in one page.
#
# No compiler, no header, no import lib: a late-bound scripting client drives
# the same hubs the C++ smoke test does, purely through IDispatch and the
# registered type library.
#
# Two hubs in ONE process talking over Dmx, so nothing needs a TCP port.
# A dmx:// dial does not retry, so the listener is armed first.
#
# ON EVENTS.  This script does NOT sink _IP2PHubEvents, and that is a host
# limitation rather than a gap in the layer: .NET (so PowerShell) can only bind
# COM events through an interop assembly for the coclass, which needs TlbImp /
# an early-bound reference.  An early-bound client -- the C++ ComSmokeTest, VB6,
# or C# with an interop for TargetComLib -- attaches to the same connection
# point and gets OnMessage/OnPeerUp/OnPeerDown/OnError.  What the script CAN
# observe end to end is delivery: IsPeerUp only goes True after the kernel's
# login handshake completes, and Broadcast returns True only if at least one
# peer was up to take a copy.
#
# That limitation is exactly why the READ SIDE (RelationTo / ConCount / PeerAt /
# EndpointFor / Description) exists, and this script is the tier it was added
# for: no return code -- IDispatch discards success HRESULTs -- and no event
# either.  Asking the hub afterwards is the only channel left, and it is one
# every tier has.
#
#   .\run_com_smoke.ps1 -KeepRegistered     # register the DLL per-user
#   .\script_client.ps1
#   regsvr32 /u /n /i:user "..\..\out\x64\Debug\TargetCom.dll"

[CmdletBinding()]
param([ValidateSet('Debug','Release')] [string] $Config = 'Debug')

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$testDir = Join-Path $here "..\..\out\x64\$Config"

# The kernel DLLs resolve against the PROCESS directory, so run from where
# run_com_smoke.ps1 staged them.
Push-Location $testDir

$script:fails = 0
function Check($ok, $what) {
    if ($ok) { Write-Host "  ok    $what" }
    else     { Write-Host "  FAIL  $what"; $script:fails++ }
}

try {
    $net = New-Object -ComObject TargetCom.P2PNetwork
    Check ($null -ne $net) "New-Object -ComObject TargetCom.P2PNetwork"
    Write-Host "        $($net.VersionString)"
    Check ($net.MaxPayload -eq 24576) "MaxPayload reads as a property"

    $server = $net.CreateHub("Script.Server")
    $client = $net.CreateHub("Script.Client")
    Check ($server.Address -eq "Script.Server") "hub.Address reads as a property"

    # One pair of verbs for every transport, with the transport in the string.
    # Swap "dmx://ScriptDemo" for "tcp://:7799" / "tcp://127.0.0.1:7799" and
    # this script runs across machines with no other edit -- which is the whole
    # point of the endpoint being a value rather than a method name.
    $server.Listen("Script.Client", "dmx://ScriptDemo")   # arm first: Dmx never retries
    $client.Connect("Script.Server", "dmx://ScriptDemo")

    $deadline = (Get-Date).AddSeconds(15)
    while (-not $client.IsPeerUp("Script.Server") -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 50
    }
    Check ($client.IsPeerUp("Script.Server")) "Dmx link completed the login handshake"
    Check ($server.IsPeerUp("Script.Client")) "...and the listening side agrees"

    # THE READ SIDE, and why it exists.
    #
    # Both arms above returned P2PF_S_UNRELATED_LINK -- "Script.Server" and
    # "Script.Client" are SIBLINGS, so the link is armed and carries direct
    # traffic, but nothing can ever be ROUTED through it and a broadcast will
    # not relay beyond it.  A script sees none of that: IDispatch normalises
    # every success code to S_OK, these two methods have no retval for it to
    # arrive in, and this host cannot sink OnError either.  Three channels, all
    # shut.  So the question is asked AFTERWARDS instead -- which works from
    # here, from cscript, from VBA and from C# alike.
    $p2pfRelUnrelated = 0x0800
    Check ((($server.RelationTo("Script.Client")) -band $p2pfRelUnrelated) -ne 0) `
          "RelationTo says this edge is a sibling one: direct traffic only"
    Check ($server.ConCount -ge 1) "ConCount reads as a property"
    Check ($server.PeerAt(0) -eq "Script.Client") "PeerAt(0) names the peer"
    Check ($server.EndpointFor("Script.Client") -eq "dmx://ScriptDemo") `
          "EndpointFor gives back the endpoint that was armed"

    # One call, one string, straight into a log or a bug report.
    Write-Host "        --- Description ---"
    ($server.Description -split "`n") | Where-Object { $_ } | ForEach-Object { Write-Host "        $_" }

    $client.SendText("Script.Server", "greeting", "hello from PowerShell")
    Check $true "SendText accepted a string with no marshalling ceremony"

    # A byte payload from script: any array PowerShell can hand over as
    # SAFEARRAY(VT_UI1).
    $bytes = [byte[]](1..64)
    $client.Send("Script.Server", "bin", $bytes)
    Check $true "Send accepted a byte[] as SAFEARRAY(VT_UI1)"

    # True only if a peer was actually up to receive a copy -- real delivery.
    Check ($server.Broadcast("news", "to everyone") -eq $true) `
          "Broadcast returned True, so a peer took a copy"

    # HRESULTs surface as COMExceptions with the facade's own codes intact.
    # (.NET keeps HResult as a signed Int32, so compare the formatted value.)
    function HResultOf([scriptblock] $sb) {
        try { & $sb; return '(no error)' }
        catch { return '0x{0:X8}' -f $_.Exception.HResult }
    }

    Check ((HResultOf { $client.SendText("Script.Server", "P2Pmsg_Nope", "x") }) -eq '0x80040205') `
          "reserved topic surfaces as P2PF_E_RESERVED_TOPIC (0x80040205)"

    # ...and the code no longer arrives naked.  The object implements
    # ISupportErrorInfo, so every failure leaves an IErrorInfo behind; that is
    # what VBScript reads as Err.Description and what .NET puts in the
    # exception message.  It matters most for the ENDPOINT, which is a string:
    # nothing at the call site is typed, so a typo is only ever a runtime
    # failure, and "0x80040208" alone does not say which part was wrong.
    function MessageOf([scriptblock] $sb) {
        try { & $sb; return '(no error)' } catch { return $_.Exception.Message }
    }

    $why = MessageOf { $server.Listen("Script.Peer", "tpc://:7799") }
    Check ($why -match 'tpc://:7799' -and $why -match 'tcp://') `
          "a mistyped endpoint explains itself: the string back, and the grammar"
    Write-Host "        --- Err.Description ---"
    Write-Host "        $why"

    # Both arguments are strings, so only the facade's guard catches a swap --
    # and now it says which slot was wrong rather than just "invalid".
    $why = MessageOf { $server.Connect("dmx://ScriptDemo", "Script.Client") }
    Check ($why -match 'PEER slot') "a swapped-argument call names the offending slot"

    # LINK -- the verb only the network can offer, and until now the one piece
    # of the flat ABI a script could not reach.  It arms the LISTENING side
    # first, which is the ordering rule this tier had no way to learn: the two
    # arms at the top of this script had to be written in that order by hand,
    # because a dmx:// dial does not retry.  One call, both ends, and the
    # endpoint omitted entirely -- the facade derives an in-process service
    # name from the address pair.
    $a = $net.CreateHub("Script.Link")
    $b = $net.CreateHub("Script.Link.Peer")
    $net.Link("Script.Link", "Script.Link.Peer")

    $deadline = (Get-Date).AddSeconds(15)
    while (-not $a.IsPeerUp("Script.Link.Peer") -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 50
    }
    Check ($a.IsPeerUp("Script.Link.Peer")) "Link armed both ends from one call"
    Check ($a.EndpointFor("Script.Link.Peer").StartsWith("dmx://")) `
          "...over an in-process endpoint nobody had to configure"
    # Parent and child, so this edge CAN be routed through -- unlike the
    # sibling pair above.  Same question, opposite answer.
    $p2pfRelDescendant = 0x0200
    Check ((($a.RelationTo("Script.Link.Peer")) -band $p2pfRelDescendant) -ne 0) `
          "RelationTo says this one is a descendant edge: routable"

    Check ((HResultOf { $net.Link("Script.Link", "Script.NoSuchHub") }) -eq '0x8004020A') `
          "Link to an unknown address is P2PF_E_NO_HUB (0x8004020A)"

    $b.Close()
    $a.Close()

    # A SECURE HUB -- the piece this tier could not have had any other way.
    # Everything behind CreateSecureHub is an ECDSA identity, its publishable
    # point, an ECDH agreement key, a three-column allow-list, a revocation
    # list and the kernel's arming gate, and a script can express none of it.
    # So it is not exposed; it is arranged.  One verb, and the hub that comes
    # back demands a SIGNED LOGIN from every peer it links to.
    #
    # Note the verb that did NOT change: Link is still Link, with the same
    # arguments.  Authentication is a property of a HUB -- enforcement is
    # hub-wide in the kernel with no per-connection override -- so it is
    # settled where the hub is made, before it can have a connection at all.
    $secA = $net.CreateSecureHub("Script.Sec")
    $secB = $net.CreateSecureHub("Script.Sec.Peer")

    # BEFORE ANY LINK.  The hub holds its keys from the moment it exists, and
    # requires nothing yet: an allow-list that lists nobody refuses everybody,
    # so the kernel will not start such a hub at all.  Enforcement goes on with
    # the first peer it can authenticate.
    Check ($secA.SecurityInfo -match 'signs=1' -and $secA.SecurityInfo -match 'required=0') `
          "a fresh secure hub holds its keys and requires nothing yet"

    $net.Link("Script.Sec", "Script.Sec.Peer")

    $deadline = (Get-Date).AddSeconds(15)
    while (-not $secA.IsPeerUp("Script.Sec.Peer") -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 50
    }
    Check ($secA.IsPeerUp("Script.Sec.Peer")) `
          "the peer came up, so the SIGNED login completed both ways"

    # THE CHECK THAT MATTERS, and it is armed=1 rather than required=1: a hub
    # can require authentication and be unable to perform it, and that state
    # refuses every peer rather than authenticating any.  A secure hub that had
    # quietly fallen back to a plain one would pass the IsPeerUp check above
    # exactly as this one does.
    Check ($secA.SecurityInfo -match 'required=1' -and $secA.SecurityInfo -match 'armed=1') `
          "...and the hub requires auth AND can enforce it"
    Write-Host "        --- SecurityInfo ---"
    Write-Host "        $($secA.SecurityInfo)"

    # The fingerprint is what an operator reads down a phone line to confirm
    # that the key which arrived is the key that was sent.  It is never an
    # identifier this code trusts -- a trust decision is made against the full
    # public point, in the allow-list.
    Check ($secA.SecurityInfo -ne $secB.SecurityInfo) `
          "each hub has its own identity fingerprint"

    # BOTH SECURE OR NEITHER.  A secure hub demands a login a plain one holds
    # no key to produce, so the mixed pair is refused before anything is armed
    # rather than armed into a link that could never come up.  Script.Link was
    # closed above, so Script.Cfg's pair is not up yet -- use the plain server
    # this script opened with.
    Check ((HResultOf { $net.Link("Script.Sec", "Script.Client") }) -eq '0x80040218') `
          "a secure hub cannot be linked to a plain one (p2pfSecurity)"
    $why = MessageOf { $net.Link("Script.Sec", "Script.Client") }
    Check ($why -match 'CreateSecureHub') `
          "...and the message says which verb makes both ends match"

    # A PLAIN hub answers the same question rather than raising: asking whether
    # something is secure should not be an exception.
    Check ($server.SecurityInfo -match 'required=0' -and $server.SecurityInfo -match '\(none\)') `
          "a plain hub reads back as holding no identity at all"

    $secB.Close()
    $secA.Close()

    # THE DEPLOYMENT MAP -- the piece this tier wanted most.  A script could
    # only ever arm something by spelling the endpoint at the call site, so
    # moving a server meant editing the script.  Now the script says who talks
    # to whom and a text file says where everything lives; in production that
    # string comes from FileSystemObject.OpenTextFile(...).ReadAll().
    $map = @"
# peers.ini -- where each address lives
Script.Cfg = TCP://127.0.0.1:7814

; the dialer gets this verbatim, the listener gets it with the host dropped
"@
    $net.SetEndpointMap($map)
    Check ($net.EndpointFor("Script.Cfg") -eq "tcp://127.0.0.1:7814") `
          "SetEndpointMap took a here-string and canonicalised it"
    Check ($net.EndpointFor("Script.Nobody") -eq "") `
          "...and an unconfigured address reads back empty, not as an error"

    # Neither Listen nor Connect names an endpoint, and the link is TCP.
    $cfgS = $net.CreateHub("Script.Cfg")
    $cfgC = $net.CreateHub("Script.Cfg.Client")
    $cfgS.Listen("Script.Cfg.Client", "")
    $cfgC.Connect("Script.Cfg", "")

    $deadline = (Get-Date).AddSeconds(15)
    while (-not $cfgC.IsPeerUp("Script.Cfg") -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 50
    }
    Check ($cfgC.IsPeerUp("Script.Cfg")) `
          "Listen('',...) and Connect('',...) linked up from the map alone"
    Check ($cfgS.EndpointFor("Script.Cfg.Client") -eq "tcp://:7814") `
          "...the listener armed the map entry with the host dropped"

    # A bad line names the LINE NUMBER.  That is why the map validates when it
    # is set: from a script the alternative is an endpoint failure surfacing
    # later, from a Connect, about a file this call never mentions.
    $why = MessageOf { $net.SetEndpointMap("A = tcp://h:1`nB = htp://nope`n") }
    Check ($why -match 'line 2') "a malformed map line is reported by line number"
    Write-Host "        $why"
    Check ($net.EndpointFor("Script.Cfg") -eq "tcp://127.0.0.1:7814") `
          "...and the map that was already loaded is untouched"

    $cfgC.Close()
    $cfgS.Close()
    $net.SetEndpointMap("")
    Check ($net.EndpointFor("Script.Cfg") -eq "") "empty text clears the map"

    $client.Close()
    $server.Close()

    Check ((HResultOf { $server.Listen("Script.Client", "dmx://ScriptDemo") }) -eq '0x80040206') `
          "a closed hub surfaces P2PF_E_CLOSED (0x80040206)"

    [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($net)
}
finally {
    Pop-Location
}

Write-Host "`n$script:fails failure(s)"
exit $script:fails
