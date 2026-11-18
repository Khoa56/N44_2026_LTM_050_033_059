@echo off
setlocal EnableExtensions DisableDelayedExpansion
if /i "%~1"=="--help" goto usage
if "%~1"=="/?" goto usage
pushd "%~dp0" || exit /b 1

wsl.exe -d Ubuntu-22.04 --exec true
if errorlevel 1 goto missing_wsl
if /i "%~1"=="--setup" goto setup
set "PX4_MONITOR_IP=%~1"
if defined PX4_MONITOR_IP goto run
echo Nhap IPv4 may Khoa neu Khoa dung may khac.
echo Neu Khoa chay cung Ubuntu WSL, nhap 127.0.0.1.
set /p "PX4_MONITOR_IP=IPv4 may Khoa (Enter neu Khoa chay tren Windows cung may): "
if not defined PX4_MONITOR_IP set "PX4_MONITOR_IP=auto"

:run
set "WSLENV=PX4_MONITOR_IP/u:%WSLENV%"
wsl.exe -d Ubuntu-22.04 --exec bash Khoi2_UAV_PX4_Son/scripts/5_test_tich_hop.sh
if errorlevel 1 goto failed
echo Nhat ky chuyen bay nam trong test-son-bay.csv.
goto done

:setup
wsl.exe -d Ubuntu-22.04 --exec bash Khoi2_UAV_PX4_Son/scripts/2_setup_px4_mavsdk_ubuntu.sh
if errorlevel 1 goto failed
echo Da cai xong. Chay lai Son.bat de test.
goto done

:missing_wsl
echo Can WSL2 Ubuntu-22.04. Trong PowerShell Admin, chay wsl --install -d Ubuntu-22.04.
echo Sau do mo Ubuntu, tao tai khoan, roi chay Son.bat --setup.

:failed
echo.
echo Son chua chay xong. Xem thong bao phia tren.
pause
popd
exit /b 1

:done
pause
popd
exit /b 0

:usage
echo Son.bat [IPv4 ^| --setup]
echo Mo PX4 SITL, bat luong MAVLink cho Khoa va chay mot luot bay mo phong.
exit /b 0
