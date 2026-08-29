@echo off
rem ============================================================
rem  PoqEditor (imgui single-file) build script
rem    Uses VS 18 2026 + MSVC /MT static runtime
rem    Output 1: build\Release\PoqEditor.exe    (single exe, no DLL deps)
rem    Output 2 (pack only): build\Release\PoqEditor\ + .7z
rem  用法:
rem    build.bat            默认只编译(不打包),日常开发用
rem    build.bat pack       编译 + 打包(7z)
rem ============================================================
setlocal
set ROOT=%~dp0
set BUILD=%ROOT%build
set RELEASE=%BUILD%\Release

rem 参数: pack = 编译+打包; 缺省 = 只编译
set PACK=0
if /i "%~1"=="pack" set PACK=1

rem imgui dir (default C:/env/imgui-1.92.1, override here if needed)
set IMGUI_DIR=C:/env/imgui-1.92.1
rem 7z executable for max-compression packaging (adjust path if needed)
set SZIP=C:\MyPrograms\7-Zip\7z.exe

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERR] cmake not found in PATH, use "C:\Program Files\CMake\bin"
    set "PATH=C:\Program Files\CMake\bin;%PATH%"
)
if "%PACK%"=="0" goto :build
if not exist "%SZIP%" (
    echo [ERR] 7z.exe not found: %SZIP%
    goto :err
)
:build

echo [1/2] configuring with Visual Studio 18 2026 (x64) ...
cmake -S "%ROOT%." -B "%BUILD%" -G "Visual Studio 18 2026" -A x64 -DIMGUI_DIR=%IMGUI_DIR%
if errorlevel 1 goto :err

echo [2/2] building Release ...
cmake --build "%BUILD%" --config Release -j 8
if errorlevel 1 goto :err

if not exist "%RELEASE%\PoqEditor.exe" (
    echo [ERR] output missing: %RELEASE%\PoqEditor.exe
    goto :err
)

if "%PACK%"=="0" goto :done

echo [pack] packaging Release\PoqEditor\ + PoqEditor.7z (max) ...
set STAGE=%RELEASE%\PoqEditor
rmdir /s /q "%STAGE%" 2>nul
mkdir "%STAGE%" 2>nul
copy /y "%RELEASE%\PoqEditor.exe" "%STAGE%\" >nul || goto :err
copy /y "%ROOT%readme.txt" "%STAGE%\readme.txt" >nul || goto :err
copy /y "%ROOT%readme.txt" "%BUILD%\readme.txt" >nul || goto :err

rem 生成 PoqEditor.exe.sig (MD5 校验值),随包发布
set "MD5="
for /f "skip=1 tokens=1" %%i in ('certutil -hashfile "%STAGE%\PoqEditor.exe" MD5') do if not defined MD5 set "MD5=%%i"
> "%STAGE%\PoqEditor.exe.sig" echo %MD5%
echo   [sig] PoqEditor.exe.sig = %MD5%
copy /y "%ROOT%gm_cmd.txt" "%STAGE%\gm_cmd.txt" >nul || goto :err
copy /y "%ROOT%gm_cmd.txt" "%BUILD%\gm_cmd.txt" >nul || goto :err
del /q "%RELEASE%\PoqEditor.7z" 2>nul
pushd "%RELEASE%"
"%SZIP%" a -t7z -mx=9 PoqEditor.7z PoqEditor || (popd & goto :err)
popd

:done
echo.
echo ============================================================
echo  done.
echo    exe:     %RELEASE%\PoqEditor.exe
if "%PACK%"=="0" goto :shownopack
echo    folder:  %STAGE%\  (PoqEditor.exe + readme.txt + gm_cmd.txt)
echo    release: %RELEASE%\PoqEditor.7z  (max-compressed, outermost=PoqEditor)
echo    readme:  %BUILD%\readme.txt
goto :showend
:shownopack
echo    (只编译,未打包。要打包请运行: build.bat pack )
:showend
echo ============================================================
exit /b 0

:err
echo.
echo [FAIL] see errors above.
exit /b 1
