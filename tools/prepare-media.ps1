# CI only. Third-party binaries/shaders stay in the build/installed package, never git.
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$engine='https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261021/mpv-dev-x86_64-20261021-git-e471496c56.7z'
$shader='https://github.com/bloc97/Anime4K/releases/download/v4.0.1/Anime4K_v4.0.zip'
$downloads=@(
 @{Url=$engine;File='mpv.7z';Hash='efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633'},
 @{Url=$shader;File='anime4k.zip';Hash='139cd282086457c5adc79caf7b75b8b825091d71c9b54958c18745fea62d7ed7'}
)
foreach($item in $downloads){
 $file=Join-Path $Destination $item.File
 Invoke-WebRequest -Uri $item.Url -OutFile $file
 if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ine $item.Hash){throw 'Pinned media package hash mismatch'}
}
& 7z x (Join-Path $Destination 'mpv.7z') "-o$Destination" -y | Out-Null
if($LASTEXITCODE -ne 0){throw 'mpv extraction failed'}
$shaders=Join-Path $Destination 'shaders'
& 7z x (Join-Path $Destination 'anime4k.zip') "-o$shaders" -y | Out-Null
if($LASTEXITCODE -ne 0){throw 'shader extraction failed'}
@{engine=$engine;engineSha256=$downloads[0].Hash;mpvCommit='e471496c56';ffmpegCommit='9a861e968';shader=$shader;shaderSha256=$downloads[1].Hash;defaultEnhancement='off';source='https://github.com/shinchiro/mpv-winbuild-cmake/tree/20261021'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'media-provenance.json') -Encoding UTF8
# Licenses and clear corresponding-source directions travel with the binaries.
$licenses=Join-Path $Destination 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Invoke-WebRequest 'https://raw.githubusercontent.com/mpv-player/mpv/e471496c56/LICENSE.GPL' -OutFile (Join-Path $licenses 'mpv-GPL.txt')
Invoke-WebRequest 'https://raw.githubusercontent.com/FFmpeg/FFmpeg/9a861e968/COPYING.GPLv3' -OutFile (Join-Path $licenses 'ffmpeg-GPLv3.txt')
Invoke-WebRequest 'https://raw.githubusercontent.com/bloc97/Anime4K/v4.0.1/LICENSE' -OutFile (Join-Path $licenses 'anime4k-MIT.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\THIRD-PARTY.md') -Destination $licenses
