@echo off
rem ============================================================
rem  PoqEditor (imgui single-file) build script
rem    Uses VS 18 2026 + MSVC /MT static runtime
rem    Output 1: build\Release\PoqEditor.exe    (single exe, no DLL deps)
rem    Output 2 (pack only): build\Release\PoqEditor\ + .7z
rem  �÷�:
rem    build.bat            Ĭ��ֻ����(�����),�ճ�������
rem    build.bat pack       ���� + ���(7z)
rem ============================================================
setlocal
set ROOT=%~dp0
set BUILD=%ROOT%build
set RELEASE=%BUILD%\Release

rem ����: pack = ����+���; ȱʡ = ֻ����
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

rem [0/2] delete old exe files before each build (avoid stale-inject reuse)
del /q "%RELEASE%\*.exe" 2>nul
del /q "%RELEASE%\PoqEditor\*.exe" 2>nul
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

rem ���� PoqEditor.exe.sig (MD5 У��ֵ),�������
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
echo    (ֻ����,δ�����Ҫ���������: build.bat pack )
:showend
echo ============================================================
exit /b 0

:err
echo.
echo [FAIL] see errors above.
exit /b 1
