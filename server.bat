@echo off
cd /d "%~dp0"
rem Eigenstaendiger Server ohne Fenster. Beispiel: server.bat --name "Mein Server" --port 27015 --max 8
build\worldridr.exe --server %*
