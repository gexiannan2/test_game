@echo off

set "proj_dir=%cd%"
echo "handle dir: %proj_dir%"

%~d0
cd %~dp0
echo "tool dir: %cd%"

:: 生成代码文件
::python ./script/generate.py %proj_dir%"
call "./dist/generate.exe" "%proj_dir%" || (
    exit /b !errorlevel!
)

:: 暂停1s
:: timeout /t 1

:: 修改代码文件
::python ./script/modify.py %proj_dir%"
call "./dist/modify.exe" "%proj_dir%" || (
    exit /b !errorlevel!
)


cd %proj_dir%"
