@echo off
setlocal EnableExtensions DisableDelayedExpansion
if /i "%~1"=="--help" goto usage
if "%~1"=="/?" goto usage
pushd "%~dp0ESP32-C3-LTMProject-Tuan" || exit /b 1
set "port="
set "action=upload"
if /i "%~1"=="--build" set "action=build"
if /i "%~1"=="--config" goto edit_config
if "%~1"=="" goto locate_pio
if "%action%"=="build" goto locate_pio
set "port=%~1"

:locate_pio
set "pio_exe="
for /f "delims=" %%I in ('where pio.exe 2^>nul') do if not defined pio_exe set "pio_exe=%%I"
if not defined pio_exe if defined PLATFORMIO_CORE_DIR if exist "%PLATFORMIO_CORE_DIR%\penv\Scripts\pio.exe" set "pio_exe=%PLATFORMIO_CORE_DIR%\penv\Scripts\pio.exe"
if not defined pio_exe if exist "%USERPROFILE%\.platformio\penv\Scripts\pio.exe" set "pio_exe=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not defined pio_exe goto missing_pio
if exist "..\.deps\platformio\packages\toolchain-riscv32-esp\bin\riscv32-esp-elf-g++.exe" if exist "..\.deps\platformio\platforms\espressif32\platform.json" for %%I in ("..\.deps\platformio") do set "PLATFORMIO_CORE_DIR=%%~fsI"
if defined PLATFORMIO_CORE_DIR if exist "%PLATFORMIO_CORE_DIR%\." for %%I in ("%PLATFORMIO_CORE_DIR%") do set "PLATFORMIO_CORE_DIR=%%~fsI"
for %%I in (".") do set "firmware_dir=%%~fsI"
if "%action%"=="build" goto build

if not exist "include\network.local.h" goto configure_and_continue
goto check_config

:configure_and_continue
call :open_config
if errorlevel 1 goto failed
echo Luu file trong Notepad, cam ESP32, roi nhan phim bat ky.
pause >nul
goto check_config

:edit_config
call :open_config
if errorlevel 1 goto failed
goto done

:open_config
if not exist "include\network.local.h" copy "include\network.example.h" "include\network.local.h" >nul
if not exist "include\network.local.h" exit /b 1
echo Dien Wi-Fi 2.4 GHz va IPv4 may Khoa vao include\network.local.h.
start "" notepad.exe "%CD%\include\network.local.h"
exit /b %errorlevel%

:check_config
findstr /l /c:"TEN_WIFI_2_4_GHZ" /c:"MAT_KHAU_WIFI" /c:"DIA_CHI_IPV4_MAY_KHOA" "include\network.local.h" >nul
if errorlevel 2 goto failed
if not errorlevel 1 goto incomplete_config
echo Nap ESP32-C3, sau do mo Serial 115200.
if defined port goto upload_with_port
"%pio_exe%" run -d "%firmware_dir%" -e esp32_c3_usb -t upload
if errorlevel 1 goto upload_failed
"%pio_exe%" device monitor -d "%firmware_dir%" -e esp32_c3_usb -b 115200
goto finished

:upload_with_port
"%pio_exe%" run -d "%firmware_dir%" -e esp32_c3_usb -t upload --upload-port "%port%"
if errorlevel 1 goto upload_failed
"%pio_exe%" device monitor -d "%firmware_dir%" -e esp32_c3_usb --port "%port%" -b 115200
goto finished

:build
"%pio_exe%" run -d "%firmware_dir%" -e esp32_c3_usb
goto finished

:incomplete_config
echo Chua dien xong network.local.h. Chay Tuan.bat --config de mo lai.
goto failed

:upload_failed
echo Kiem tra cap USB va dong Serial Monitor khac.
echo Neu can chon cong, chay lai vi du Tuan.bat COM6.
"%pio_exe%" device list
goto failed

:missing_pio
echo Can cai PlatformIO IDE trong VS Code hoac mo terminal PlatformIO roi chay lai.
goto failed

:finished
if not errorlevel 1 goto done
:failed
echo.
echo Tuan chua chay xong. Xem thong bao phia tren.
pause
popd
exit /b 1

:done
popd
exit /b 0

:usage
echo Tuan.bat [COMx ^| --build ^| --config]
echo Mac dinh nap ESP32-C3 qua USB va mo Serial. Ctrl+C de dong Serial.
exit /b 0
