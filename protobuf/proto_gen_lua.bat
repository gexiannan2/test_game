@echo off

set "proj_dir=%cd%"
echo "handle dir: %proj_dir%"

%~d0
cd %~dp0
echo "tool dir: %cd%"

::: generate lua pb files
call "./dist/generate_lua.exe" "%proj_dir%"

::: post-process: Msg_Svr only keeps svc_ prefixed server protocols
set "svr_file=%proj_dir%\protos\Msg_Svr_protobuf.lua"
if exist "%svr_file%" (
    echo -- msg reg (svc_ only> "%svr_file%.tmp"
    echo return {>> "%svr_file%.tmp"
    findstr /r /c:"    svc_" "%svr_file%" >> "%svr_file%.tmp"
    echo }>> "%svr_file%.tmp"
    move /y "%svr_file%.tmp" "%svr_file%" >nul
)

cd %proj_dir%"
