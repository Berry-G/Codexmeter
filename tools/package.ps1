param([switch]$Portable)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Explicit allowlist: never recursively archive a working directory or dist.
$publicFiles = @('.gitignore','README.md','LICENSE','Codexmeter.example.ini','build.ps1',
    'src/main.cpp','src/quota.hpp','src/renderer.hpp','tests/tests.cpp',
    'tools/probe.ps1','tools/package.ps1','vendor/json.hpp','vendor/LICENSE.json',
    'docs/images/titlebar.svg','docs/images/weekly-light.png',
    'docs/images/weekly-dark.png','docs/images/two-windows.png')
function Write-Package($destination, $entries) {
    $stream = [IO.File]::Open($destination, [IO.FileMode]::Create)
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $entries) {
            $source = Join-Path $root $entry.Source
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing package input: $($entry.Source)" }
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $source, $entry.Name, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $zip.Dispose(); $stream.Dispose() }
    Get-Item -LiteralPath $destination | Select-Object Name,Length
}
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
$sourceEntries = $publicFiles | ForEach-Object { @{ Source=$_; Name=$_ } }
Write-Package (Join-Path $root 'dist/Codexmeter-source.zip') $sourceEntries
if ($Portable) {
    $entries = @(@{Source='dist/Codexmeter.exe';Name='Codexmeter.exe'},
        @{Source='Codexmeter.example.ini';Name='Codexmeter.ini'},
        @{Source='vendor/LICENSE.json';Name='vendor/LICENSE.json'},
        @{Source='README.md';Name='README.md'},
        @{Source='LICENSE';Name='LICENSE'})
    $entries += $publicFiles | Where-Object { $_ -like 'docs/images/*' } | ForEach-Object { @{Source=$_;Name=$_} }
    Write-Package (Join-Path $root 'dist/Codexmeter-portable.zip') $entries
}
