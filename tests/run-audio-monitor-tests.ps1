param([switch]$SkipNativeBuild)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio with the C++ desktop workload is required.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (!$SkipNativeBuild) {
    & $msbuild (Join-Path $repo 'Project Files\Source\ArtemisSDR.sln') /t:ChannelMaster /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
    if ($LASTEXITCODE) { throw 'Native audio build failed.' }
}
& $msbuild (Join-Path $PSScriptRoot 'AudioMonitorTests.vcxproj') /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if ($LASTEXITCODE) { throw 'Audio test build failed.' }
$savedPath = $env:PATH
try {
    $env:PATH = (Join-Path $repo 'Project Files\bin\x64\Release') + ';' + $savedPath
    Push-Location (Join-Path $repo 'artifacts\audio-tests')
    try {
        & '.\AudioMonitorTests.exe'
        if ($LASTEXITCODE) { throw "Audio monitor tests failed: $LASTEXITCODE" }
    } finally { Pop-Location }
} finally { $env:PATH = $savedPath }
