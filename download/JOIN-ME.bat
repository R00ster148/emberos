@echo off
cd /d "%~dp0"
echo Joining EmberOS 3.0 in: %CD%
echo.
for %%n in (001 002 003 004 005 006 007 008 009) do (
  if not exist "emberos-3.0.iso.%%n" (
    echo PROBLEM: emberos-3.0.iso.%%n is missing from this folder.
    echo.
    echo If you opened the ZIP without unzipping it, right-click the ZIP,
    echo choose "Extract All...", then run JOIN-ME.bat from the unzipped
    echo "download" folder.
    echo.
    pause
    exit /b 1
  )
)
copy /b emberos-3.0.iso.001+emberos-3.0.iso.002+emberos-3.0.iso.003+emberos-3.0.iso.004+emberos-3.0.iso.005+emberos-3.0.iso.006+emberos-3.0.iso.007+emberos-3.0.iso.008+emberos-3.0.iso.009 emberos-3.0.iso >nul
if errorlevel 1 (
  echo PROBLEM: joining failed. Is the disk full? The ISO needs about 800 MB.
  pause
  exit /b 1
)
echo Checking the file (should end in ...8caa3c9):
certutil -hashfile emberos-3.0.iso SHA256
echo.
echo Done! emberos-3.0.iso is in: %CD%
explorer /select,"%CD%\emberos-3.0.iso"
pause
