<#
    Copyright 2026 Khrustal & Mann
                 MELBOURNE, VICTORIA, AUSTRALIA, 3000

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
    implied. See the License for the specific language governing
    permissions and limitations under the License.


    check_api_drift.ps1 -- fail when an upstream C++ API has gained something the
    binding layer over it never heard about.

    THE DIRECTION THIS CHECKS IS THE ONE NOTHING ELSE WATCHES. check_exports.ps1
    catches the surface WIDENING -- a symbol leaving the DLL that no manifest
    declared. It cannot see the opposite and quieter failure: the C++ grows a
    capability, the binding does not follow, and the flat surface goes on
    reporting the same healthy figure it reported last month. Nobody gets an
    error, because nothing is wrong with what exists; what is wrong is what does
    not.

    That happened here. 0278981 added P3PmsgBSTR's adopting length-validated
    constructor -- "the one the receive path needs", by its own commit message --
    and touched no file under the binding layer. The export manifests were red
    within the hour, for the mangled half. The flat C ABI stayed at 282 and said
    nothing at all, which was correct and useless in the same breath.

    WHAT IT ACTUALLY DOES, in one sentence: for each configured PAIR of surfaces
    it enumerates the public members upstream, asks whether each one has a
    counterpart downstream, and fails on any that does not unless the allowlist
    says why not.

    A PAIR is two surfaces and the rule that maps between them. The pairs live in
    a config file rather than here, because the three MSCS repositories that need
    this check have three different downstream surfaces -- a flat C ABI, and two
    COM servers speaking IDL -- and only the mapping differs. See
    api-drift.config.psd1 next to this script.

    THE ALLOWLIST IS THE PRODUCT, NOT THE SCAN. Enumerating members is easy. What
    this repository does not currently have, anywhere, is a written statement of
    which C++ is MEANT to be reachable from C -- that exists only as whatever
    Msgcore_c.cpp happens to contain, which is a record of what somebody did, not
    of what anybody decided. Every allowlist entry is one line of that statement,
    and the reason field is the whole point: an entry without one is a silenced
    check, exactly as md-citations.allow says of its own.

    A FIRST RUN IS RED AND LARGE, and that is not a defect in the check. Many
    public members are legitimately internal -- every operator overload, every
    constructor taking a P2PmsgHANDLE, everything with an MFC type in its
    signature. -Seed banks them all as UNTRIAGED so the check can go green today
    and the backlog becomes a thing you can grep for rather than a thing you
    remember. UNTRIAGED is counted out loud on EVERY run, in its own line, for
    the reason the register keeps relearning: a number nobody prints is a number
    nobody reads.

    WHAT IT CANNOT DO. It compares NAMES. It cannot see that a binding's
    behaviour drifted -- msgcore_mgr_load answering differently after 0278981 is
    invisible here, because the signature never moved. Only a test catches that,
    and no test caught it. It is also not a C++ parser: the extractor is
    line-based, which works because these headers are written in an unusually
    regular style, and it mis-reads templates and macro-heavy declarations. Those
    go in the allowlist with a reason saying it was the extractor, not the design.
#>

param(
    # The pairs to check. Defaults to the config beside this script.
    [string]$Config = 'tools/ci/api-drift.config.psd1',

    # Bank every currently-unbound member as UNTRIAGED and exit 0. For the first
    # run only. It does not make anything true -- it makes the backlog visible
    # and greppable instead of a wall of red nobody triages.
    [switch]$Seed,

    # Report every member and how it matched, not just the failures.
    [switch]$ShowBound
)

$ErrorActionPreference = 'Stop'

$repo = (git rev-parse --show-toplevel 2>$null)
if (-not $repo) { Write-Error 'not inside a git repository'; exit 2 }
$repo = $repo.Trim()
Set-Location $repo

if (-not (Test-Path $Config)) {
    Write-Error "config not found: $Config"
    exit 2
}
$cfg = Import-PowerShellDataFile (Resolve-Path $Config)
$allowFile = if ($cfg.AllowFile) { $cfg.AllowFile } else { 'tools/ci/api-drift.allow' }

# ------------------------------------------------------------ extractors ----

# C++ headers. Line-based on purpose: a real parse needs libclang, which is a
# toolchain dependency this check is not worth. The style these headers are
# written in -- return type on its own line, name at a fixed indent, spaced
# parens -- is regular enough that the residue is small and lands in the
# allowlist where a human can see it was the extractor's fault.
function Get-CxxTypes([string[]]$files) {
    $types = @{}
    foreach ($file in $files) {
        if (-not (Test-Path $file)) { Write-Error "no such file: $file"; exit 2 }
        $lines = [System.IO.File]::ReadAllLines((Join-Path $repo $file))

        $stack   = [System.Collections.Generic.List[object]]::new()
        $depth   = 0
        $pending = $null      # a class header seen, brace not yet met
        $lineNo  = 0

        foreach ($raw in $lines) {
            $lineNo++
            # Strip line comments and the simple one-line block comment. Block
            # comments spanning lines are left alone: they declare no members,
            # and the keyword guard below rejects their prose.
            $line = $raw -replace '//.*$', '' -replace '/\*.*?\*/', ''
            if ($line -match '^\s*#') { continue }

            $opens  = ([regex]::Matches($line, '\{')).Count
            $closes = ([regex]::Matches($line, '\}')).Count

            # class | struct [EXPORT_MACRO] Name [: bases] [{]
            if ($line -match '^\s*(?<kw>class|struct)\s+(?:[A-Za-z_][A-Za-z0-9_]*\s+)?(?<n>[A-Za-z_][A-Za-z0-9_]*)\s*(?<tail>[:{].*)?$' -and
                $line -notmatch ';\s*$') {
                $pending = [pscustomobject]@{
                    Name   = $Matches.n
                    Access = $(if ($Matches.kw -eq 'struct') { 'public' } else { 'private' })
                    Depth  = $depth
                }
                if (-not $types.ContainsKey($pending.Name)) {
                    $types[$pending.Name] = [System.Collections.Generic.List[object]]::new()
                }
            }

            if ($pending -and $opens -gt 0) {
                $stack.Add($pending)
                $pending = $null
            }

            if ($stack.Count -gt 0) {
                $top = $stack[$stack.Count - 1]

                if ($line -match '^\s*(?<a>public|protected|private)\s*:') {
                    $top.Access = $Matches.a
                }

                # Members of the type itself only -- one brace deep. A nested
                # class or an inline body is somebody else's surface.
                if ($top.Access -eq 'public' -and $depth -eq ($top.Depth + 1)) {
                    $name = $null
                    if ($line -match 'operator\s*(?<op>[^\s(]+)\s*\(') {
                        $name = 'operator' + $Matches.op
                    }
                    elseif ($line -match '(?<n>~?[A-Za-z_][A-Za-z0-9_]*)\s*\(') {
                        $cand = $Matches.n
                        $kw = @('if','for','while','switch','return','sizeof','catch',
                                'throw','new','delete','static_cast','reinterpret_cast',
                                'const_cast','dynamic_cast','defined','assert','ASSERT')
                        if ($kw -notcontains $cand) { $name = $cand }
                    }
                    if ($name) {
                        $kind = if ($name -eq $top.Name) { 'ctor' }
                                elseif ($name -eq ('~' + $top.Name)) { 'dtor' }
                                elseif ($name -like 'operator*') { 'operator' }
                                else { 'method' }
                        $types[$top.Name].Add([pscustomobject]@{
                            Name = $name; Kind = $kind; File = $file; Line = $lineNo
                            Text = $raw.Trim()
                        })
                    }
                }
            }

            $depth += $opens - $closes
            while ($stack.Count -gt 0 -and $depth -le $stack[$stack.Count - 1].Depth) {
                $stack.RemoveAt($stack.Count - 1)
            }
            if ($depth -lt 0) { $depth = 0 }
        }
    }
    return $types
}

# A flat C header: every function carrying the API macro.
function Get-CFunctions([string[]]$files, [string]$macro) {
    $names = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    foreach ($file in $files) {
        if (-not (Test-Path $file)) { Write-Error "no such file: $file"; exit 2 }
        $text = [System.IO.File]::ReadAllText((Join-Path $repo $file))
        $esc = [regex]::Escape($macro)
        $rx = [regex]("$esc\s+(?:[A-Za-z_][A-Za-z0-9_]*[\s\*&]+)*?(?<n>[A-Za-z_][A-Za-z0-9_]*)\s*\(")
        foreach ($m in $rx.Matches($text)) { [void]$names.Add($m.Groups['n'].Value) }
    }
    return $names
}

# An IDL: methods per interface, so a pair can map one interface to one type.
function Get-IdlInterfaces([string[]]$files) {
    $ifaces = @{}
    foreach ($file in $files) {
        if (-not (Test-Path $file)) { Write-Error "no such file: $file"; exit 2 }
        $lines = [System.IO.File]::ReadAllLines((Join-Path $repo $file))
        $cur = $null; $depth = 0; $pending = $null; $lineNo = 0
        foreach ($raw in $lines) {
            $lineNo++
            $line = $raw -replace '//.*$', ''
            # interface IFooCom : IDispatch   (a forward declaration ends in ';')
            if ($line -match '^\s*(?:dispinterface|interface)\s+(?<n>[A-Za-z_][A-Za-z0-9_]*)' -and
                $line -notmatch ';\s*$') {
                $pending = $Matches.n
                if (-not $ifaces.ContainsKey($pending)) {
                    $ifaces[$pending] = [System.Collections.Generic.List[object]]::new()
                }
            }
            $opens  = ([regex]::Matches($line, '\{')).Count
            $closes = ([regex]::Matches($line, '\}')).Count
            if ($pending -and $opens -gt 0) { $cur = $pending; $pending = $null; $depth = 0 }
            if ($cur) {
                # A return type then a name then '('. NOT 'HRESULT' specifically:
                # an event method on a dispinterface returns void, and matching
                # only HRESULT reported every OnMessage / OnPeerUp / OnTimer in
                # _IP2PHubEvents as unbound -- 8 phantom gaps out of a first-run
                # backlog of 32, in the one interface whose whole job is the part
                # of the surface a scripting client cannot poll for.
                # An attribute line ([id(1), helpstring("...")]) has no leading
                # identifier pair, so it does not match.
                if ($line -match '^\s*(?<r>[A-Za-z_][A-Za-z0-9_:\*]*)\s+(?<n>[A-Za-z_][A-Za-z0-9_]*)\s*\(') {
                    $ifaces[$cur].Add([pscustomobject]@{
                        Name = $Matches.n; File = $file; Line = $lineNo; Text = $raw.Trim()
                    })
                }
                $depth += $opens - $closes
                if ($depth -le 0 -and $closes -gt 0) { $cur = $null }
            }
        }
    }
    return $ifaces
}

# ---------------------------------------------------------------- naming ----

function ConvertTo-Snake([string]$n) {
    $s = [regex]::Replace($n, '(?<=[a-z0-9])(?=[A-Z])', '_')
    $s = [regex]::Replace($s, '(?<=[A-Z])(?=[A-Z][a-z])', '_')
    return $s.ToLowerInvariant()
}

# -------------------------------------------------------------- allowlist ----

$allowed   = @{}    # "pair<NUL>Type::Member" -> reason
$allowSeen = @{}
if (Test-Path $allowFile) {
    $lineNo = 0
    foreach ($line in Get-Content $allowFile) {
        $lineNo++
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith('#')) { continue }
        if ($t -notmatch '^(?<p>\S+)\s+(?<m>\S+)\s*(?:#\s*(?<why>.*))?$') {
            Write-Error "${allowFile}:${lineNo}: cannot parse: $t"
            exit 2
        }
        $key = $Matches.p + "`0" + $Matches.m
        $allowed[$key]   = $(if ($Matches.why) { $Matches.why } else { '(no reason given)' })
        $allowSeen[$key] = $false
    }
}

# ------------------------------------------------------------------ check ----

$unbound   = @()
$redundant = @()
$boundCnt  = 0
$scanned   = 0
$untriaged = 0

foreach ($pair in $cfg.Pairs) {
    $pairId = $pair.Id
    $up = Get-CxxTypes $pair.Upstream.Files

    $surfaceNames = $null; $ifaces = $null
    switch ($pair.Surface.Kind) {
        'cfn' { $surfaceNames = Get-CFunctions $pair.Surface.Files $pair.Surface.ApiMacro }
        'idl' { $ifaces       = Get-IdlInterfaces $pair.Surface.Files }
        default { Write-Error "pair '$pairId': unknown Surface.Kind '$($pair.Surface.Kind)'"; exit 2 }
    }

    foreach ($typeName in $pair.TypeMap.Keys) {
        if (-not $up.ContainsKey($typeName)) {
            Write-Host "  WARNING  pair '$pairId' maps $typeName, which no upstream header declares."
            continue
        }
        $target = $pair.TypeMap[$typeName]

        # Overloads collapse to one name upstream; the surface usually spells
        # them out (DeclareItem -> declare_double, declare_bool, declare_wstr).
        # Asking about each distinct NAME once is the question that has an
        # answer; asking about each overload is a question no surface can be
        # expected to mirror.
        $seen = @{}
        foreach ($mem in $up[$typeName]) {
            if ($seen.ContainsKey($mem.Name)) { continue }
            $seen[$mem.Name] = $true
            $scanned++

            $key = "$pairId`0$typeName::$($mem.Name)"

            $hit = $null
            if ($pair.Surface.Kind -eq 'cfn') {
                $snake = ConvertTo-Snake $mem.Name
                $exact = $target + $snake
                if ($surfaceNames.Contains($exact)) { $hit = $exact }
                else {
                    $root = $target + ($snake -split '_')[0]
                    foreach ($n in $surfaceNames) {
                        if ($n -eq $root -or $n.StartsWith($root + '_')) { $hit = $n; break }
                    }
                }
            }
            else {
                if ($ifaces.ContainsKey($target)) {
                    $cand = @($ifaces[$target] | ForEach-Object { $_.Name })
                    if ($cand -contains $mem.Name) { $hit = $mem.Name }
                    else {
                        # An accessor upstream is routinely a property downstream:
                        # GetText -> Text. Reported as such, so the match is
                        # auditable rather than merely permissive.
                        $stripped = $mem.Name -replace '^(Get|Set|Put)', ''
                        if ($stripped -and $cand -contains $stripped) { $hit = "$stripped (property)" }
                    }
                }
            }

            # The allowlist is consulted AFTER the match, not before it. Checking
            # it first is cheaper and wrong: an entry whose member has since
            # gained a binding would never be looked at again, and the file would
            # accumulate exemptions for things that are no longer exempt. That is
            # the same defect md-citations.allow reports as a stale entry, and it
            # is worth more here, because an UNTRIAGED line quietly covering a
            # member somebody has since bound makes the backlog read longer than
            # it is.
            if ($allowed.ContainsKey($key)) {
                $allowSeen[$key] = $true
                if ($hit) {
                    $redundant += [pscustomobject]@{
                        Pair = $pairId; Member = "$typeName::$($mem.Name)"; Hit = $hit
                    }
                }
                elseif ($allowed[$key] -match '^UNTRIAGED') { $untriaged++ }
                continue
            }

            if ($hit) {
                $boundCnt++
                if ($ShowBound) { Write-Host ("  bound    {0,-46} -> {1}" -f "$typeName::$($mem.Name)", $hit) }
            }
            else {
                $unbound += [pscustomobject]@{
                    Pair = $pairId; Type = $typeName; Member = $mem.Name; Kind = $mem.Kind
                    File = $mem.File; Line = $mem.Line; Target = $target; Text = $mem.Text
                }
            }
        }
    }
}

# ---------------------------------------------------------------- seeding ----

if ($Seed -and $unbound.Count -gt 0) {
    $out = [System.Collections.Generic.List[string]]::new()
    $out.Add('')
    $out.Add('# ---------------------------------------------------------------------------------')
    $out.Add('#  UNTRIAGED -- banked by -Seed, not decided by anybody.')
    $out.Add('#')
    $out.Add('#  Each line below says only that the member had no counterpart on the day the')
    $out.Add('#  check was first run. Replacing UNTRIAGED with a real reason IS the triage, and')
    $out.Add('#  the count is printed on every run so the backlog cannot go quiet.')
    $out.Add('# ---------------------------------------------------------------------------------')
    foreach ($u in ($unbound | Sort-Object Pair, Type, Member)) {
        $out.Add(("{0}  {1}::{2}    # UNTRIAGED ({3}, {4}:{5})" -f
                  $u.Pair, $u.Type, $u.Member, $u.Kind, $u.File, $u.Line))
    }
    Add-Content -Path $allowFile -Value $out
    Write-Host ''
    Write-Host "Seeded $($unbound.Count) UNTRIAGED entries into $allowFile."
    Write-Host '  Nothing has been decided. Run again without -Seed to see the check green,'
    Write-Host "  then work the backlog down:  Select-String UNTRIAGED $allowFile"
    exit 0
}

# ----------------------------------------------------------------- report ----

Write-Host ("Scanned {0} public members over {1} pair(s); {2} bound, {3} allowlisted." -f
            $scanned, @($cfg.Pairs).Count, $boundCnt, $allowed.Count)
if ($untriaged -gt 0) {
    Write-Host ("  {0} of the allowlist entries are UNTRIAGED -- banked, not decided." -f $untriaged)
    if ($env:GITHUB_ACTIONS) { Write-Host "::notice file=$allowFile::$untriaged UNTRIAGED api-drift entries" }
}

if ($redundant.Count -gt 0) {
    Write-Host ''
    Write-Host "$($redundant.Count) allowlist entr$(if ($redundant.Count -eq 1) {'y is'} else {'ies are'}) no longer needed -- the member has a binding now:"
    foreach ($r in $redundant) {
        Write-Host ("    {0}  {1}   -> {2}" -f $r.Pair, $r.Member, $r.Hit)
    }
    Write-Host "  Drop the line from $allowFile; the exemption outlived the gap."
}

$stale = $allowSeen.GetEnumerator() | Where-Object { -not $_.Value }
if ($stale) {
    Write-Host ''
    Write-Host "$($stale.Count) allowlist entr$(if (@($stale).Count -eq 1) {'y'} else {'ies'}) no longer match anything:"
    foreach ($s in $stale) {
        $p, $m = $s.Key -split "`0"
        Write-Host "    $p  $m"
    }
    Write-Host "  The member is gone or was renamed; drop the line from $allowFile."
}

if ($unbound.Count -eq 0) {
    Write-Host ''
    Write-Host 'OK -- every upstream member has a binding, or an allowlisted reason not to.'
    exit 0
}

Write-Host ''
Write-Host ("{0} upstream members reach no binding:" -f $unbound.Count)
Write-Host ''
foreach ($u in $unbound) {
    Write-Host ("  {0}:{1}" -f $u.File, $u.Line)
    Write-Host ("      {0}::{1}   ({2})" -f $u.Type, $u.Member, $u.Kind)
    Write-Host ("      looked for : {0}*" -f $u.Target)
    if ($env:GITHUB_ACTIONS) {
        Write-Host "::error file=$($u.File),line=$($u.Line)::$($u.Type)::$($u.Member) has no binding in the $($u.Pair) surface"
    }
}
Write-Host ''
Write-Host '  Three ways out, in order of preference:'
Write-Host '    1. Write the binding, if a caller on that surface should reach it.'
Write-Host "    2. Add a line to $allowFile WITH A REASON, if the member is"
Write-Host '       deliberately internal. The reason is the statement this project'
Write-Host '       does not otherwise have anywhere.'
Write-Host '    3. If the extractor misread the declaration, say THAT in the reason,'
Write-Host '       so the next reader knows it is a tooling limit and not a decision.'
exit 1
