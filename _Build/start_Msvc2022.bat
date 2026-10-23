@ECHO OFF

REM Auto-detect Visual Studio installation using vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSVC="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -property installationPath`) do set "MSVC=%%i"
)

if not defined MSVC (
    REM Fallback paths for common VS installations
    if exist "C:\Program Files\Microsoft Visual Studio\18\Community" (
        set "MSVC=C:\Program Files\Microsoft Visual Studio\18\Community"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community" (
        set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional" (
        set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Professional"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise" (
        set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    ) else (
        echo ERROR: Could not find Visual Studio installation
        pause
        exit /b 1
    )
)

echo Using Visual Studio at: %MSVC%

set "PATH=C:\Windows\system32;C:\Windows;%MSVC%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd"
set "VCVARSALL=%MSVC%\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARSALL%" x64
start "" "%MSVC%\Common7\IDE\devenv.exe" ..
