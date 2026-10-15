@echo off
chcp 65001 >nul
rem Builds VolumeGuard.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
cd /d "%~dp0"
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%CSC%" (
  echo Не найден компилятор .NET Framework 4.
  pause
  exit /b 1
)
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /win32icon:assets\app.ico /out:VolumeGuard.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Core.dll src\*.cs
if errorlevel 1 (
  echo Сборка не удалась.
  pause
  exit /b 1
)
echo Готово: VolumeGuard.exe
pause
