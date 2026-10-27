@echo off
rem Launches SUMMIT straight into the game (no editor). First launch compiles shaders and can take a while.
set "UE-LocalDataCachePath=E:\UnrealDDC"
start "" "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" "%~dp0..\..\Summit.uproject" -game -windowed -ResX=1600 -ResY=900
