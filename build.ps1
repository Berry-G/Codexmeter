param([switch]$Test,[switch]$TestsOnly)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force dist | Out-Null
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio or Build Tools with Desktop development with C++.' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC C++ Build Tools are required.' }
    $dev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    # Import the installed toolchain environment into this process, without installing anything.
    $bootstrap = Join-Path $PSScriptRoot 'dist\toolchain-env.cmd'
    "@echo off`r`ncall `"$dev`" -arch=x64 -host_arch=x64 >nul`r`nset" | Set-Content -LiteralPath $bootstrap -Encoding ascii
    $vars = & $env:ComSpec /d /c dist\toolchain-env.cmd
    foreach ($line in $vars) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') }
    }
    $common = @('/nologo','/std:c++17','/O2','/W4','/EHsc','/MT','/utf-8','/DWIN32_LEAN_AND_MEAN','/DNOMINMAX')
    if (-not $TestsOnly) {
        & cl.exe @common 'src/main.cpp' '/Fo:dist/main.obj' '/Fe:dist/Codexmeter.exe' '/link' '/SUBSYSTEM:WINDOWS' 'user32.lib' 'gdi32.lib' 'shell32.lib'
        if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
        # Never overwrite personal settings, or copy them into a public package.
        if (-not (Test-Path dist/Codexmeter.ini)) {
            Copy-Item Codexmeter.example.ini dist/Codexmeter.ini
        }
        Copy-Item vendor/LICENSE.json dist/LICENSE.json -Force
        Copy-Item LICENSE dist/LICENSE -Force
    }
    if ($Test -or $TestsOnly) {
        & cl.exe @common 'tests/tests.cpp' '/Fo:dist/tests.obj' '/Fe:dist/tests.exe' '/link' 'user32.lib' 'gdi32.lib' 'shell32.lib'
        if ($LASTEXITCODE -ne 0) { throw 'Test build failed' }
        & ./dist/tests.exe
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
    }
    if (-not $TestsOnly) { Get-Item dist/Codexmeter.exe | Select-Object FullName,Length }
} finally { Pop-Location }
