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
# run_com_smoke.ps1 -- stage, register per-user, run ComSmokeTest, unregister.
#
# Per-user registration (regsvr32 /i:user, i.e. our DllInstall) writes to
# HKCU\Software\Classes, so no elevation is needed and nothing machine-wide is
# touched.  The registration is always removed again, pass or fail.
#
# The x64 and Win32 servers share every GUID -- as they must, since a ProgID
# means one thing whatever the caller's bitness -- but they do NOT collide:
# HKCU\Software\Classes\CLSID is registry-redirected, so the 64-bit view holds
# the x64 InprocServer32 and the WOW6432Node view holds the Win32 one, and the
# shared TypeLib key carries both under its win32/win64 subkeys.  Registering
# one platform therefore never disturbs the other.  Debug vs Release DOES
# collide, because those share a view -- hence the unregister below.
#
#   .\run_com_smoke.ps1                                 # Debug|x64
#   .\run_com_smoke.ps1 -Config Release
#   .\run_com_smoke.ps1 -Platform Win32                 # the 32-bit server
#   .\run_com_smoke.ps1 -Platform Win32 -Config Release
#   .\run_com_smoke.ps1 -KeepRegistered                 # leave it for a script client

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string] $Config   = 'Debug',
    [ValidateSet('x64','Win32')]     [string] $Platform = 'x64',
    [switch] $KeepRegistered
)

$ErrorActionPreference = 'Stop'
$here      = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo      = (Resolve-Path (Join-Path $here "..\..\..")).Path   # the MSCS root
# The COM server, its test exe and the facade all build into one shared output
# folder now, so what used to be three sibling directories is a single path.
$outDir    = Join-Path $here "..\..\out\$Platform\$Config"

# Artifact names carry no debug 'd' suffix in either configuration -- only the
# directory keeps Debug and Release apart, as everywhere else in this tree.
$comDll   = Join-Path $outDir "TargetCom.dll"
$exe      = Join-Path $outDir "ComSmokeTest.exe"

foreach ($p in @($comDll, $exe)) {
    if (-not (Test-Path $p)) { throw "missing $p -- build the $Config|$Platform configuration first" }
}

# The facade's post-build step stages Msgcore.dll and Targetcore.dll into the
# shared output folder, so normally everything the test exe and the COM server
# need is already beside them and nothing is copied here at all. The fallbacks
# cover a tree where that step did not run: IW_CustomBuildStep.bat deploys both
# platforms into bin\<Config><arch> (bin\Debug32, bin\Debug64), and each
# kernel's own output root is probed after that, so a tree built with
# WDMSCS_VSUTILS unset -- which disables deployment entirely -- still runs
# instead of failing on a missing DLL.
function Resolve-Dep([string] $name, [string[]] $candidates) {
    foreach ($c in $candidates) {
        $p = Join-Path $c $name
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    throw "missing $name for $Config|$Platform -- looked in:`n  " + ($candidates -join "`n  ")
}

$binCfg = "$Config" + $(if ($Platform -eq 'x64') { '64' } else { '32' })
$deps = @(
    (Resolve-Dep 'TargetFacade.dll' @($outDir)),
    (Resolve-Dep 'Targetcore.dll'   @($outDir, "$repo\bin\$binCfg", "$repo\Targetcore\out\$Platform\$Config")),
    (Resolve-Dep 'Msgcore.dll'      @($outDir, "$repo\bin\$binCfg", "$repo\Msgcore\$Platform\$Config"))
)

# One destination now: the loading process (the test exe) and the COM DLL that
# regsvr32 loads with an altered search path sit in the same directory.
foreach ($d in $deps) {
    if ((Split-Path $d -Parent) -ne (Resolve-Path $outDir).Path) { Copy-Item $d $outDir -Force }
}

# regsvr32 must match the server's bitness: the 32-bit one writes the
# WOW6432Node view, the 64-bit one writes the native view.  From a 32-bit
# PowerShell, System32 file-system-redirects to SysWOW64, so the 64-bit binary
# has to be reached through the Sysnative alias instead.
$native = if ([Environment]::Is64BitProcess) { "$env:SystemRoot\System32" } else { "$env:SystemRoot\Sysnative" }
$regsvr = if ($Platform -eq 'x64') { "$native\regsvr32.exe" } else { "$env:SystemRoot\SysWOW64\regsvr32.exe" }

Write-Host "registering (per-user, $Platform) $comDll"
$reg = Start-Process $regsvr -ArgumentList '/s','/n','/i:user',"`"$comDll`"" -Wait -PassThru
if ($reg.ExitCode -ne 0) { throw "regsvr32 failed with $($reg.ExitCode)" }

try {
    Write-Host "running $exe`n"
    & $exe
    $rc = $LASTEXITCODE
} finally {
    if (-not $KeepRegistered) {
        Start-Process $regsvr -ArgumentList '/s','/u','/n','/i:user',"`"$comDll`"" -Wait | Out-Null
        Write-Host "`nunregistered"
    } else {
        Write-Host "`nleft registered as ProgID TargetCom.P2PNetwork (per-user, $Platform)"
    }
}

Write-Host "exit code $rc"
exit $rc
