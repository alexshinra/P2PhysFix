param([string]$Zig = "$env:LOCALAPPDATA\zig\zig-x86_64-windows-0.17.1\zig.exe")
# Builds P2PhysFix.asi (32-bit DLL) into the "build" folder next to this script's folder.
#   zig rc version.rc version.res
#   zig cc -target x86-windows-gnu -shared -O2 -s -Wall -Wextra -o P2PhysFix.asi p2physfix.c version.res -lkernel32 -luser32
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'p2physfix.c'
$rc  = Join-Path $PSScriptRoot 'version.rc'
$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$res = Join-Path $out 'version.res'
$dll = Join-Path $out 'P2PhysFix.asi'
Push-Location $out
try {
  & $Zig rc $rc $res
  if ($LASTEXITCODE -ne 0) { throw "zig rc failed ($LASTEXITCODE)" }
  & $Zig cc -target x86-windows-gnu -shared -O2 -s -Wall -Wextra -o $dll $src $res -lkernel32 -luser32
  if ($LASTEXITCODE -ne 0) { throw "zig cc failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Get-ChildItem -LiteralPath $out | Where-Object { $_.Extension -in '.pdb', '.lib', '.res' } | Remove-Item -Force
$b = [IO.File]::ReadAllBytes($dll); $pe = [BitConverter]::ToInt32($b, 0x3c)
$vi = (Get-Item -LiteralPath $dll).VersionInfo
"built {0} ({1:N0} bytes, machine 0x{2:x}, version {3})" -f $dll, $b.Length, [BitConverter]::ToUInt16($b, $pe + 4), $vi.FileVersion
