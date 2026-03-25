@echo off
setlocal
set SCRIPT_DIR=%~dp0

py -3 "%SCRIPT_DIR%viewer_mcp_client.py" %* 2>nul
if %ERRORLEVEL% EQU 0 goto :eof

python "%SCRIPT_DIR%viewer_mcp_client.py" %*
