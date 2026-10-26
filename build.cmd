@echo off
rem ------------------------------------------------------------------------------------------
rem  Build the KevFM radio firmware + web-files image into .\build
rem  Uses the arduino-cli that comes with Arduino IDE 2.x and the esp32 core 3.3.12.
rem ------------------------------------------------------------------------------------------
setlocal
set HERE=%~dp0
set CLI=C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
set CFG=%USERPROFILE%\.arduinoIDE\arduino-cli.yaml
set MKSPIFFS=%LOCALAPPDATA%\Arduino15\packages\esp32\tools\mkspiffs\0.2.3\mkspiffs.exe
set FQBN=esp32:esp32:esp32:PartitionScheme=min_spiffs,PSRAM=disabled,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,DebugLevel=none

python "%HERE%tools\embed_web.py"
if errorlevel 1 ( echo PAGE EMBED FAILED & exit /b 1 )

"%CLI%" --config-file "%CFG%" compile --fqbn %FQBN% --output-dir "%HERE%build" "%HERE%yoRadio"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

"%MKSPIFFS%" -c "%HERE%yoRadio\data" -b 4096 -p 256 -s 0x60000 "%HERE%build\spiffs.bin"
if errorlevel 1 ( echo SPIFFS IMAGE FAILED & exit /b 1 )

echo.
echo Build OK. Now: flash.cmd COMx   (first time)   or   flash.cmd COMx app   (update)
endlocal
