@echo off
rem ============================================================
rem  e996 protobuf one-click generator
rem  Scans protos/ subdir for *.proto, generates .pb.cc / .pb.h
rem  into protos/c++/, then injects msg registration code via
rem  dist/modify.exe (calls the original proto_gen.bat logic).
rem ============================================================

cd /d "%~dp0"
set "TOOL_DIR=%CD%"
set "PROTO_DIR=%TOOL_DIR%\protos"
set "OUT_DIR=%PROTO_DIR%\c++"
set "LUA_OUT_DIR=%PROTO_DIR%\lua"

if not exist "%PROTO_DIR%" (
    echo [ERROR] protos dir not found: %PROTO_DIR%
    pause
    exit /b 1
)

rem cleanup old output dirs before generate (no backup, direct remove)
if exist "%OUT_DIR%"     rd /s /q "%OUT_DIR%" 2>nul
if exist "%LUA_OUT_DIR%"  rd /s /q "%LUA_OUT_DIR%" 2>nul

rem run the original tool - it scans protos/ and generates c++ code
call "%TOOL_DIR%\proto_gen.bat"
if errorlevel 1 (
    echo [ERROR] proto_gen.bat failed, errorlevel=%errorlevel%
    exit /b %errorlevel%
)

rem optionally generate lua bindings
if exist "%TOOL_DIR%\proto_gen_lua.bat" (
    call "%TOOL_DIR%\proto_gen_lua.bat"
    if errorlevel 1 (
        echo [WARN] proto_gen_lua.bat failed, errorlevel=%errorlevel%
    )
)

echo.
echo ============================================================
echo  done. output:
echo    c++ : %OUT_DIR%
echo    lua : %LUA_OUT_DIR%
echo ============================================================
exit /b 0
