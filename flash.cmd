@echo off
rem ------------------------------------------------------------------------------------------
rem  Flash a KevFM radio over USB-C.
rem
rem    flash.cmd COM5          first time: firmware + web page files (erases saved Wi-Fi)
rem    flash.cmd COM5 app      update: firmware only (keeps Wi-Fi, volume and light mode)
rem
rem  Find the COM port in Device Manager > Ports ("Silicon Labs CP210x ... (COM5)").
rem  The ELEGOO board normally enters download mode by itself. If esptool says
rem  "Failed to connect", hold the BOOT button, start this again, release BOOT when
rem  "Connecting..." appears.
rem ------------------------------------------------------------------------------------------
setlocal
if "%~1"=="" (
  echo Usage: flash.cmd COMx [app]
  exit /b 1
)
set PORT=%~1
set B=%~dp0build
set ESPTOOL=%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe
set BOOTAPP0=%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.12\tools\partitions\boot_app0.bin

for %%F in ("%B%\yoRadio.ino.bootloader.bin" "%B%\yoRadio.ino.partitions.bin" "%B%\yoRadio.ino.bin" "%ESPTOOL%" "%BOOTAPP0%") do (
  if not exist %%F ( echo Missing %%F - build first ^(see README^) & exit /b 1 )
)

if /i "%~2"=="app" (
  "%ESPTOOL%" --chip esp32 --port %PORT% --baud 921600 write-flash -z 0x10000 "%B%\yoRadio.ino.bin"
  goto :done
)

if not exist "%B%\spiffs.bin" ( echo Missing %B%\spiffs.bin - run tools\make_spiffs.sh & exit /b 1 )
"%ESPTOOL%" --chip esp32 --port %PORT% --baud 921600 write-flash -z ^
  0x1000   "%B%\yoRadio.ino.bootloader.bin" ^
  0x8000   "%B%\yoRadio.ino.partitions.bin" ^
  0xe000   "%BOOTAPP0%" ^
  0x10000  "%B%\yoRadio.ino.bin" ^
  0x390000 "%B%\spiffs.bin"

:done
if errorlevel 1 ( echo FLASH FAILED & exit /b 1 )
echo.
echo Done. Open the Serial Monitor at 115200 baud to watch it boot.
endlocal
