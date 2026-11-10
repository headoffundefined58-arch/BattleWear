@echo off
setlocal
REM BattleWear - build script (VS2022 generator; deliberately does NOT call vcvars).
REM MSBuild resolves the toolchain on its own, which sidesteps two known blockers on
REM this machine: vcvars64.bat calls reg.exe (blocked by policy) and duplicated
REM HTTP_PROXY/http_proxy env vars break CL.exe startup.

set ROOT=%~dp0
set BUILD=%ROOT%build

set CMAKE=
for %%P in (
  "%ProgramFiles%\CMake\bin\cmake.exe"
  "D:\dev\tools\cmake\cmake-4.2.0-windows-x86_64\bin\cmake.exe"
  "D:\VS2022\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) do if exist %%P if not defined CMAKE set CMAKE=%%~P

where cmake >nul 2>&1 && if not defined CMAKE set CMAKE=cmake

if not defined CMAKE (
  echo [BattleWear] cmake not found. Install CMake or pass its path manually.
  exit /b 1
)

echo [BattleWear] cmake = %CMAKE%
if not exist "%BUILD%\CMakeCache.txt" (
  "%CMAKE%" -B "%BUILD%" -S "%ROOT%." -G "Visual Studio 17 2022" -A x64
  if errorlevel 1 exit /b 1
)
"%CMAKE%" --build "%BUILD%" --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo [BattleWear] artifacts:
dir /b "%BUILD%\bin\Release"
echo.
echo [BattleWear] run conformance:
echo   "%BUILD%\bin\Release\bw_tests.exe"
endlocal
