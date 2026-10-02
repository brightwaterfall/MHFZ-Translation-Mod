# Fetch Mogapedia EN translation payload (gunzip to .json for the mod).
param(
	[string]$OutDir = (Join-Path $PSScriptRoot "..\data")
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$gz = Join-Path $OutDir "translations-en.json.gz"
$json = Join-Path $OutDir "translations-en.json"
Write-Host "Downloading translations-en.json.gz ..."
gh release download v0.3.0 -R Mogapedia/MHFrontier-Translation -p "translations-en.json.gz" -D $OutDir --clobber
# gunzip via .NET
$in = [System.IO.File]::OpenRead($gz)
$gzip = New-Object System.IO.Compression.GzipStream($in, [System.IO.Compression.CompressionMode]::Decompress)
$out = [System.IO.File]::Create($json)
$gzip.CopyTo($out)
$out.Close(); $gzip.Close(); $in.Close()
Write-Host "Wrote $json ($((Get-Item $json).Length) bytes)"
