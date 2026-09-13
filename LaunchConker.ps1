param([switch]$BuildOnly, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$launcherRoot = $PSScriptRoot
$launcherPackaged = Join-Path $launcherRoot 'conker-launcher.exe'
if (-not $BuildOnly -and -not $Rebuild -and (Test-Path -LiteralPath $launcherPackaged)) {
    Start-Process -FilePath $launcherPackaged -WorkingDirectory $launcherRoot -WindowStyle Normal
    return
}
$launcherCmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $launcherCmake) {
    $launcherVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $launcherVswhere) {
        $launcherCmake = & $launcherVswhere -latest -products '*' -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    }
}
if (-not $launcherCmake) { throw 'Install CMake and Visual Studio with Desktop development with C++.' }
$launcherBuild = Join-Path $launcherRoot 'build\launcher'
& $launcherCmake -S (Join-Path $launcherRoot 'src\launcher') -B $launcherBuild -A x64 -DCONKER_LAUNCHER_TEST=OFF
if ($LASTEXITCODE -ne 0) { throw 'Launcher configuration failed.' }
& $launcherCmake --build $launcherBuild --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }
if (-not $BuildOnly) {
    # The graphical launcher is the requested visible application. Its helper
    # tasks use hidden windows and write their output under the local profile.
    Start-Process -FilePath (Join-Path $launcherBuild 'Release\conker-launcher.exe') -WorkingDirectory $launcherRoot -WindowStyle Normal
}
