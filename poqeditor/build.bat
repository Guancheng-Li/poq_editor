@echo off
rem 构建 PoqEditor（需要 Windows + .NET 8 SDK）
rem 安装 .NET 8 SDK: https://dotnet.microsoft.com/download
cd /d "%~dp0"
echo [1/2] 还原依赖...
dotnet restore
if errorlevel 1 goto :fail
echo [2/2] 编译 Release...
dotnet build -c Release
if errorlevel 1 goto :fail
echo.
echo 构建成功。产物:
echo   bin\Release\net8.0-windows\PoqEditor.exe
echo.
echo 如需单文件发布(便于分发):
echo   dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true
exit /b 0
:fail
echo 构建失败。请确认已安装 .NET 8 SDK。
exit /b 1
