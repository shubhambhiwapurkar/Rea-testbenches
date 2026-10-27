# One-click project setup on a Windows workstation with Unreal Engine installed.
#   powershell -ExecutionPolicy Bypass -File Tools\setup\setup_project.ps1 [-Engine "C:\Program Files\Epic Games\UE_5.8"]
# 1. copies the UE template mannequin (placeholder climber body) into Content/
# 2. generates the Everest heightmap if Tools/terrain has not been run
# 3. builds the editor modules
# 4. downloads free textures (Poly Haven CC0 + Copernicus Sentinel-2) and builds the landscape material
# 5. retunes the mannequin walk for mountaineering speeds
# 6. runs the SummitSetup commandlet, which builds /Game/Maps/L_Everest
param(
    [string]$Engine = "C:\Program Files\Epic Games\UE_5.8"
)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path "$PSScriptRoot\..\.."
$Project = Join-Path $Root "Summit.uproject"

if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
    throw "Close the Unreal Editor first: it locks the game modules being rebuilt."
}

Write-Host "== 1/6 Mannequin"
$Mannequin = Join-Path $Root "Content\Characters\Mannequins"
if (-not (Test-Path $Mannequin)) {
    Copy-Item "$Engine\Templates\TemplateResources\High\Characters\Content\Mannequins" $Mannequin -Recurse
}

Write-Host "== 2/6 Terrain"
$Terrain = Join-Path $Root "Tools\terrain"
if (-not (Test-Path "$Terrain\output\everest\everest.r16")) {
    $Py = "$Terrain\.venv\Scripts\python.exe"
    if (-not (Test-Path $Py)) {
        python -m venv "$Terrain\.venv"
        & $Py -m pip install -r "$Terrain\requirements.txt"
    }
    & $Py "$Terrain\summit_terrain.py" fetch --peak-id everest --extent-km 20
    & $Py "$Terrain\summit_terrain.py" build --peak-id everest --extent-km 20 --ue-size 4033 --detail --fit-landmarks
    if ($LASTEXITCODE -ne 0) { throw "Terrain build failed" }
}

Write-Host "== 3/6 Build editor modules"
& "$Engine\Engine\Build\BatchFiles\Build.bat" SummitEditor Win64 Development "-Project=$Project" -WaitMutex
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$Cmd = "$Engine\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
Write-Host "== 4/6 Textures"
& "$Terrain\.venv\Scripts\python.exe" -I "$Root\Tools\setup\fetch_textures.py" --terrain "$Terrain\output\everest" --out "$Root\Tools\setup\textures"
if ($LASTEXITCODE -ne 0) { throw "Texture download failed" }
& $Cmd $Project -run=pythonscript "-script=$Root\Tools\setup\build_landscape_material.py" -unattended -nosplash
if ($LASTEXITCODE -ne 0) { throw "Landscape material build failed" }

Write-Host "== 5/6 Mannequin walk tuning"
& $Cmd $Project -run=pythonscript "-script=$Root\Tools\setup\fix_locomotion.py" -unattended -nosplash -nullrhi
if ($LASTEXITCODE -ne 0) { throw "Locomotion tuning failed" }

Write-Host "== 6/6 Build L_Everest"
& $Cmd $Project -run=SummitSetup -unattended -nosplash -nopause
if ($LASTEXITCODE -ne 0) { throw "SummitSetup commandlet failed" }

Write-Host "Done. Open Summit.uproject and press Play (Alt+P)."
