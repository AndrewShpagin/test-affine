@echo off
setlocal
cd /d "%~dp0"
echo FlowX receiver: http://127.0.0.1:8002/flowx.html
echo Configuration: config\flowx_receiver.json
echo Press Ctrl+C to stop.
flowx_receiver.exe config\flowx_receiver.json
set "receiver_exit=%errorlevel%"
if not "%receiver_exit%"=="0" pause
exit /b %receiver_exit%
