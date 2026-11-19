@echo off
setlocal EnableExtensions DisableDelayedExpansion
if /i "%~1"=="--help" goto usage
if "%~1"=="/?" goto usage
pushd "%~dp0" || exit /b 1

if /i "%~1"=="--wsl" goto wsl
if /i "%~1"=="--udp" goto udp
if not "%~1"=="" if /i not "%~1"=="--full" goto bad_option

set "sdk=%MAVSDK_ROOT%"
if not defined sdk set "sdk=%CD%\mavsdk-v3"
if exist "%sdk%\lib\cmake\MAVSDK\MAVSDKConfig.cmake" if exist "%sdk%\Release\bin\mavsdk.dll" goto full
if /i "%~1"=="--full" goto missing_sdk
echo Chua co MAVSDK Windows. Chay che do thu ESP32 qua UDP.

:udp
set "monitor=build-udp\esp32_px4_monitor.exe"
if exist "%monitor%" goto run_udp
set "monitor=build-udp\Release\esp32_px4_monitor.exe"
if exist "%monitor%" goto run_udp
set "build_dir=build-khoa-udp"
set "monitor=%build_dir%\Release\esp32_px4_monitor.exe"
call :find_cmake
if errorlevel 1 goto missing_cmake
"%cmake_exe%" -S . -B "%build_dir%" -G "Visual Studio 17 2022" -A x64 -DMONITOR_WITH_MAVSDK=OFF
if errorlevel 1 goto failed
"%cmake_exe%" --build "%build_dir%" --config Release --parallel
if errorlevel 1 goto failed

:run_udp
echo Dang nghe ESP32 tren UDP 8080. Ctrl+C de luu test-khoa-udp.csv.
"%monitor%" --csv test-khoa-udp.csv
goto finished

:full
set "PATH=%sdk%\Release\bin;%PATH%"
set "monitor=build-win\Release\esp32_px4_monitor.exe"
if exist "%monitor%" goto run_full
set "build_dir=build-khoa-full"
set "monitor=%build_dir%\Release\esp32_px4_monitor.exe"
call :find_cmake
if errorlevel 1 goto missing_cmake
"%cmake_exe%" -S . -B "%build_dir%" -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=%sdk%" -DMONITOR_WITH_MAVSDK=ON
if errorlevel 1 goto failed
"%cmake_exe%" --build "%build_dir%" --config Release --target esp32_px4_monitor --parallel
if errorlevel 1 goto failed

:run_full
echo Dang nghe ESP32 tren UDP 8080 va PX4 tren UDP 14600.
"%monitor%" --csv test-khoa-tich-hop.csv
goto finished

:wsl
wsl.exe -d Ubuntu-22.04 --exec true
if errorlevel 1 goto missing_wsl
wsl.exe -d Ubuntu-22.04 --exec bash -c "cmake -S . -B build-khoa-wsl -DCMAKE_BUILD_TYPE=Release && cmake --build build-khoa-wsl --target esp32_px4_monitor --parallel && exec ./build-khoa-wsl/esp32_px4_monitor --csv test-khoa-tich-hop.csv"
goto finished

:find_cmake
set "cmake_exe="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined cmake_exe set "cmake_exe=%%I"
if defined cmake_exe exit /b 0
if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "cmake_exe=%ProgramFiles%\CMake\bin\cmake.exe"
if defined cmake_exe exit /b 0
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" exit /b 1
for /f "usebackq delims=" %%I in (`""%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`) do if not defined cmake_exe set "cmake_exe=%%I"
if not defined cmake_exe exit /b 1
exit /b 0

:missing_cmake
echo Can Visual Studio 2022 voi Desktop development with C++ va CMake.
echo Xem muc Build tren Windows x64 trong README.md.
goto failed

:missing_sdk
echo Giai nen goi MAVSDK Windows v3.17.5 vao thu muc mavsdk-v3 theo README.md.
goto failed

:missing_wsl
echo Can WSL2 Ubuntu-22.04. Chay uav.bat --setup sau khi cai Ubuntu.
goto failed

:bad_option
echo Tuy chon khong hop le. Chay monitor.bat --help.
goto failed

:finished
if not errorlevel 1 goto done
:failed
echo.
echo Khoa chua chay xong. Xem thong bao phia tren.
pause
popd
exit /b 1

:done
popd
exit /b 0

:usage
echo monitor.bat [--udp ^| --full ^| --wsl]
echo Mac dinh uu tien MAVSDK Windows; neu chua cai thi thu UDP ESP32.
echo --udp: chi thu ESP32. --full: ESP32 va PX4 tren Windows.
echo --wsl: chay ban day du trong cung Ubuntu-22.04 voi PX4.
exit /b 0
