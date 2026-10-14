@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set MSB="C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe"

echo ============ BUILD OPUS ============
%MSB% "C:\Users\mattb\relcs-multiplayer\vendor\opus\win32\VS2015\opus.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m /v:m
if errorlevel 1 exit /b 2

echo ============ BUILD NET SLN ============
%MSB% "C:\Users\mattb\relcs-multiplayer\net\build\reLCS-net.sln" /t:net-tests;net-server;net-master;net-bot /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m /v:m
exit /b %errorlevel%
