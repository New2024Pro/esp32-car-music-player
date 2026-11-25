<#
.SYNOPSIS
  Converts audio files into MP3s the ESP32 player can play: 320 kbps, 44.1 kHz, stereo.

.DESCRIPTION
  Handles anything ffmpeg reads: MP3 at 48 kHz, M4A/AAC (including files renamed to .mp3), WAV,
  FLAC, OGG, Opus, WebM, WMA. Title / artist / album tags are kept (ID3v2.3). Cover art is dropped:
  the navi does not show it and it only makes files bigger. Emoji and characters a FAT card or an
  older navi cannot handle are removed from the file name. Originals are never changed.

.PARAMETER In
  Folder with the songs to convert.

.PARAMETER Out
  Folder for the converted MP3s (created if missing). Default: a "car" folder inside -In.

.PARAMETER Bitrate
  MP3 bit rate. Default 320k; 256k is also fine for Bluetooth SBC.

.EXAMPLE
  .\tools\convert-for-car.ps1 -In "$HOME\Music"
  Converts every song in Music into Music\car, ready to upload or copy to the SD card.
#>
param(
  [Parameter(Mandatory = $true)][string]$In,
  [string]$Out = "",
  [string]$Bitrate = "320k"
)

$ErrorActionPreference = "Stop"
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
  Write-Error "ffmpeg not found. Install it with: winget install Gyan.FFmpeg"
}
if (-not (Test-Path -LiteralPath $In -PathType Container)) { Write-Error "Folder not found: $In" }
if (-not $Out) { $Out = Join-Path $In "car" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$exts = @(".mp3", ".m4a", ".aac", ".wav", ".flac", ".ogg", ".opus", ".webm", ".wma", ".mp4")
$files = Get-ChildItem -LiteralPath $In -File | Where-Object { $exts -contains $_.Extension.ToLower() }
if (-not $files) { Write-Host "No audio files in $In"; exit 0 }

# File name for the SD card and the navi: no emoji (surrogate pairs), variation selectors or
# zero-width joiners, nothing FAT forbids, single spaces.
function Get-CarName([string]$base) {
  $s = [regex]::Replace($base, '[\uD800-\uDFFF]|\uFE0F|\uFE0E|\u200D', "")
  $s = [regex]::Replace($s, '[\\/:*?"<>|]', " ")
  $s = [regex]::Replace($s, "\s+", " ").Trim()
  if (-not $s) { $s = "Track" }
  return $s
}

$ok = 0; $failed = @()
foreach ($f in $files) {
  $name = (Get-CarName $f.BaseName) + ".mp3"
  $dest = Join-Path $Out $name
  Write-Host ("Converting {0} -> {1}" -f $f.Name, $name)
  # -map 0:a:0  first audio stream only (drops cover art / video)
  # -ar 44100   the only rate the player decodes; -ac 2 stereo
  & ffmpeg -hide_banner -loglevel error -y -i $f.FullName -map 0:a:0 -map_metadata 0 `
      -c:a libmp3lame -b:a $Bitrate -ar 44100 -ac 2 -id3v2_version 3 $dest
  if ($LASTEXITCODE -eq 0) { $ok++ } else { $failed += $f.Name }
}

Write-Host ""
Write-Host ("Done: {0} of {1} converted into {2}" -f $ok, $files.Count, $Out)
if ($failed) { Write-Host ("Failed: " + ($failed -join ", ")) -ForegroundColor Red }
Write-Host "Tip: name files 'NN - Artist - Title.mp3' (e.g. '01 - SB19 - MAPA.mp3') unless they have tags."
