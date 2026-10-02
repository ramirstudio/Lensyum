@echo off
rem Builds Lensyum and assembles a ready-to-install zip in dist\.
rem Run from "x64 Native Tools Command Prompt for VS" in the project folder.
rem Optional paths (defaults match the README):
rem   set ORT_DIR=C:\SDK\ort          (extracted Microsoft.ML.OnnxRuntime.DirectML package)
rem   set DML_DIR=C:\SDK\dml          (extracted Microsoft.AI.DirectML package)
rem   set MODEL=C:\SDK\lensyum_depth.onnx   (Depth Anything V2 Small, model.onnx renamed)

setlocal
cd /d "%~dp0"
if "%ORT_DIR%"=="" set ORT_DIR=C:\SDK\ort
if "%DML_DIR%"=="" set DML_DIR=C:\SDK\dml

echo Building...
cmake --build build --target Lensyum
if errorlevel 1 (
    echo.
    echo Build failed. Configure first, for example:
    echo   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="C:/SDK/AfterEffectsSDK"
    exit /b 1
)

set AEX=build\ae\Lensyum.aex
if not exist "%AEX%" set AEX=build\ae\Release\Lensyum.aex
if not exist "%AEX%" (
    echo Lensyum.aex not found in build\ae
    exit /b 1
)

set OUT=dist\Lensyum
if exist dist rmdir /s /q dist
mkdir "%OUT%"
copy /y "%AEX%" "%OUT%\" >nul
copy /y README.md "%OUT%\" >nul
copy /y THIRD_PARTY.md "%OUT%\" >nul
copy /y LICENSE "%OUT%\LICENSE.txt" >nul
copy /y docs\INSTALL.txt "%OUT%\" >nul

set AI=1
if exist "%ORT_DIR%\runtimes\win-x64\native\onnxruntime.dll" (
    copy /y "%ORT_DIR%\runtimes\win-x64\native\onnxruntime.dll" "%OUT%\lensyum_ort.dll" >nul
    if exist "%ORT_DIR%\runtimes\win-x64\native\onnxruntime_providers_shared.dll" copy /y "%ORT_DIR%\runtimes\win-x64\native\onnxruntime_providers_shared.dll" "%OUT%\" >nul
) else (
    echo Warning: ONNX Runtime not found in %ORT_DIR%, AI Depth files not included.
    set AI=0
)
if exist "%DML_DIR%\bin\x64-win\DirectML.dll" (
    copy /y "%DML_DIR%\bin\x64-win\DirectML.dll" "%OUT%\" >nul
) else if exist "%ORT_DIR%\runtimes\win-x64\native\DirectML.dll" (
    copy /y "%ORT_DIR%\runtimes\win-x64\native\DirectML.dll" "%OUT%\" >nul
) else (
    echo Warning: DirectML.dll not found, AI Depth will run on the CPU.
)
if not "%MODEL%"=="" if exist "%MODEL%" copy /y "%MODEL%" "%OUT%\lensyum_depth.onnx" >nul
if not exist "%OUT%\lensyum_depth.onnx" (
    if exist "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\lensyum_depth.onnx" (
        copy /y "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\lensyum_depth.onnx" "%OUT%\" >nul
    ) else (
        echo Warning: lensyum_depth.onnx not found, set MODEL=path to include it.
    )
)

powershell -NoProfile -Command "Compress-Archive -Path 'dist\Lensyum' -DestinationPath 'dist\Lensyum-1.0-win64.zip' -Force"
if errorlevel 1 exit /b 1
echo.
echo Done: dist\Lensyum-1.0-win64.zip
dir dist\Lensyum
endlocal
