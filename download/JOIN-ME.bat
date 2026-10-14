@echo off
echo Joining EmberOS 3.0...
copy /b emberos-3.0.iso.001+emberos-3.0.iso.002+emberos-3.0.iso.003+emberos-3.0.iso.004+emberos-3.0.iso.005+emberos-3.0.iso.006+emberos-3.0.iso.007+emberos-3.0.iso.008+emberos-3.0.iso.009 emberos-3.0.iso
echo.
echo Checking the file (should end in ...fbef7dc):
certutil -hashfile emberos-3.0.iso SHA256
echo.
echo Done! Use emberos-3.0.iso in VirtualBox.
pause
