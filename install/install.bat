@echo off
setlocal
rem NieR Remastered installer. Usage: install.bat ["path\to\NieR Automata folder"]
rem Copies the mod next to NieRAutomata.exe and backs up any d3d11/dxgi/dxvk.conf already there.
set "SRC=%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata"
if not exist "%GAME%\NieRAutomata.exe" (
  echo NieRAutomata.exe was not found in:
  echo   %GAME%
  set /p "GAME=Paste the game folder (the one with NieRAutomata.exe): "
)
if not exist "%GAME%\NieRAutomata.exe" (
  echo NieRAutomata.exe not found, nothing was installed.
  pause
  exit /b 1
)
if not exist "%GAME%\nier_remastered_backup" (
  mkdir "%GAME%\nier_remastered_backup"
  for %%F in (d3d11.dll dxgi.dll dxvk.conf nvngx_dlss.dll) do (
    if exist "%GAME%\%%F" copy /y "%GAME%\%%F" "%GAME%\nier_remastered_backup\" >nul
  )
)
for %%F in (d3d11.dll dxgi.dll dxvk.conf nvngx_dlss.dll) do copy /y "%SRC%%%F" "%GAME%\" >nul
echo NieR Remastered installed into %GAME%
echo In game: set Anti-Aliasing to Off, then press Page Up for the mod menu.
pause
