param([string]$Zig = "$env:LOCALAPPDATA\zig\zig-x86_64-windows-0.17.1\zig.exe")
# Builds P2PhysFix.asi (32-bit DLL) next to this script's parent folder: P2Fixer\build\P2PhysFix.asi
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'p2physfix.c'
$out = Join-Path (Split-Path $PSScriptRoot -Parent) 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$dll = Join-Path $out 'P2PhysFix.asi'
Push-Location $out
try {
  & $Zig cc -target x86-windows-gnu -shared -O2 -s -Wall -Wextra -o $dll $src -lkernel32 -luser32
  if ($LASTEXITCODE -ne 0) { throw "zig cc failed ($LASTEXITCODE)" }
} finally { Pop-Location }
Get-ChildItem -LiteralPath $out -Filter 'P2PhysFix.*' | Where-Object { $_.Extension -in '.pdb', '.lib' } | Remove-Item -Force
$b = [IO.File]::ReadAllBytes($dll); $pe = [BitConverter]::ToInt32($b, 0x3c)
"built {0} ({1:N0} bytes, machine 0x{2:x})" -f $dll, $b.Length, [BitConverter]::ToUInt16($b, $pe + 4)
