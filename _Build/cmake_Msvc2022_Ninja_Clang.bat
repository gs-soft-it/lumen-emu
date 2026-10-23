@ECHO OFF

if not !%1==! goto :with_arg

ECHO 1.Debug
ECHO 2.Debug Final
ECHO 3.Release
ECHO 4.Release Final
ECHO 5.Exit
ECHO.

CHOICE /C 12345 /M "Enter your choice:"

IF %errorlevel% EQU 5 goto :End
goto :without_arg

:with_arg
REM Map argument names to choice numbers
if /I "%1"=="Debug" set CH=1
if /I "%1"=="DebugFinal" set CH=2
if /I "%1"=="Release" set CH=3
if /I "%1"=="ReleaseFinal" set CH=4
if /I "%1"=="1" set CH=1
if /I "%1"=="2" set CH=2
if /I "%1"=="3" set CH=3
if /I "%1"=="4" set CH=4
if not defined CH (
    echo ERROR: Unknown argument %1. Use: Debug, DebugFinal, Release, ReleaseFinal
    goto :End
)
goto :Start
:without_arg
set CH=%errorlevel%
:Start

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
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" (
        set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    ) else (
        echo ERROR: Could not find Visual Studio installation
        goto :End
    )
)

echo Using Visual Studio at: %MSVC%

REM Find clang-cl in system or add common paths
REM Prioritize standard LLVM over AMD ROCm (ROCm's dev compiler may have bugs)
set "CLANG_PATH="
if exist "C:\Program Files\LLVM\bin\clang-cl.exe" (
    set "CLANG_PATH=C:\Program Files\LLVM\bin"
    echo Found standard LLVM clang
) else if exist "C:\Program Files\AMD\ROCm\6.4\bin\clang-cl.exe" (
    set "CLANG_PATH=C:\Program Files\AMD\ROCm\6.4\bin"
    echo WARNING: Using AMD ROCm clang - may have issues on non-AMD systems
)

set "PATH=C:\Windows\system32;C:\Windows;%MSVC%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd"
if defined CLANG_PATH set "PATH=%CLANG_PATH%;%PATH%"
set "VCVARSALL=%MSVC%\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VCVARSALL%" (
    echo ERROR: vcvarsall.bat not found at %VCVARSALL%
    goto :End
)

call "%VCVARSALL%" x64

REM Add clang-cl to PATH after vcvarsall (it may have reset PATH)
where clang-cl.exe >nul 2>nul
if %errorlevel% neq 0 (
    if exist "C:\Program Files\LLVM\bin\clang-cl.exe" (
        set "PATH=C:\Program Files\LLVM\bin;%PATH%"
        echo Added standard LLVM clang to PATH
    ) else if exist "C:\Program Files\AMD\ROCm\6.4\bin\clang-cl.exe" (
        set "PATH=C:\Program Files\AMD\ROCm\6.4\bin;%PATH%"
        echo Added AMD ROCm clang to PATH - WARNING: may have issues on non-AMD systems
    ) else (
        echo ERROR: clang-cl.exe not found. Please install LLVM or AMD ROCm
        goto :End
    )
)

set CFLAGS=-m64 -fdiagnostics-absolute-paths
set CXXFLAGS=-m64 -fdiagnostics-absolute-paths

REM Set Qt5 path if available
set "QT5_CMAKE_PREFIX="
if exist "C:\Qt\5.15.2\msvc2019_64\lib\cmake\Qt5" (
    set "QT5_CMAKE_PREFIX=-DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64"
)

rem @ECHO ON

GOTO choice-%CH%

:choice-1
if !%2==! (
mkdir _DebugMsvc2022NinjaClang
cd _DebugMsvc2022NinjaClang
) else (
mkdir %2
cd %2
)
set >env.txt
echo call "%VCVARSALL%" x64 >_build.bat
echo ninja >>_build.bat
echo ninja install >>_build.bat
where cmake.exe >cmake.txt
cmake -G "Ninja" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -D CMAKE_BUILD_TYPE=Debug -D CMAKE_INSTALL_PREFIX=_bin %QT5_CMAKE_PREFIX% ../../source
GOTO End

:choice-2
if !%2==! (
mkdir _DebugFinalMsvc2022NinjaClang
cd _DebugFinalMsvc2022NinjaClang
) else (
mkdir %2
cd %2
)
set >env.txt
echo call "%VCVARSALL%" x64 >_build.bat
echo ninja >>_build.bat
echo ninja install >>_build.bat
where cmake.exe >cmake.txt
cmake -G "Ninja" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -D CMAKE_BUILD_TYPE=Debug -D LUMEN_FINAL=1 -D CMAKE_INSTALL_PREFIX=_bin %QT5_CMAKE_PREFIX% ../../source
GOTO End

:choice-3
if !%2==! (
mkdir _ReleaseMsvc2022NinjaClang
cd _ReleaseMsvc2022NinjaClang
) else (
mkdir %2
cd %2
)
set >env.txt
echo call "%VCVARSALL%" x64 >_build.bat
echo ninja >>_build.bat
echo ninja install >>_build.bat
where cmake.exe >cmake.txt
cmake -G "Ninja" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX=_bin %QT5_CMAKE_PREFIX% ../../source
GOTO End

:choice-4
if !%2==! (
mkdir _ReleaseFinalMsvc2022NinjaClang
cd _ReleaseFinalMsvc2022NinjaClang
) else (
mkdir %2
cd %2
)
set >env.txt
echo call "%VCVARSALL%" x64 >_build.bat
echo ninja >>_build.bat
echo ninja install >>_build.bat
where cmake.exe >cmake.txt
cmake -G "Ninja" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -D CMAKE_BUILD_TYPE=Release -D LUMEN_FINAL=1 -D CMAKE_INSTALL_PREFIX=_bin %QT5_CMAKE_PREFIX% ../../source
GOTO End

:choice-5
:End
