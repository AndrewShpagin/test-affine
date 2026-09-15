@echo off
setlocal
cd /d "%~dp0"
echo Configuration: config\flowx_sender_windows.json
echo Set source.url to the Raspberry Pi snapshot URL before starting.
echo UDP destination: 127.0.0.1:5000 by default. Start run-receiver.cmd first.
echo Press Ctrl+C to stop.
flowx_sender.exe config\flowx_sender_windows.json %*
set "sender_exit=%errorlevel%"
if not "%sender_exit%"=="0" pause
exit /b %sender_exit%
