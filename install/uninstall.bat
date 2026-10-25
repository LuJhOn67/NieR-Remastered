@echo off
setlocal
rem NieR Remastered uninstaller. Usage: uninstall.bat ["path\to\NieR Automata folder"]
set "GAME=%~1"
if "%GAME%"=="" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\NieRAutomata"
if not exist "%GAME%\NieRAutomata.exe" set /p "GAME=Paste the game folder (the one with NieRAutomata.exe): "
if not exist "%GAME%\NieRAutomata.exe" (
  echo NieRAutomata.exe not found, nothing was removed.
  pause
  exit /b 1
)
for %%F in (d3d11.dll dxgi.dll dxvk.conf nvngx_dlss.dll nier_mod.ini) do if exist "%GAME%\%%F" del /q "%GAME%\%%F"
if exist "%GAME%\nier_remastered_backup" (
  copy /y "%GAME%\nier_remastered_backup\*" "%GAME%\" >nul 2>nul
  rmdir /s /q "%GAME%\nier_remastered_backup"
)
echo NieR Remastered removed from %GAME%
pause
