@echo off
setlocal EnableExtensions

for %%I in ("%~dp0.") do set "CARTO_SOURCE=%%~fI"

if not defined CARTO_BUILD_DIR (
    if defined LOCALAPPDATA (
        set "CARTO_BUILD_DIR=%LOCALAPPDATA%\HiddenCanopy\Cartographer\workbench-release"
    ) else (
        set "CARTO_BUILD_DIR=%TEMP%\HiddenCanopy\Cartographer\workbench-release"
    )
)
if not defined CARTO_BUILD_ATTEMPTS set "CARTO_BUILD_ATTEMPTS=3"
if not defined CARTO_BUILD_JOBS set "CARTO_BUILD_JOBS=4"

if not defined VULKAN_SDK (
    for /f "delims=" %%I in ('dir /b /ad /o-n "C:\VulkanSDK\*" 2^>nul') do if not defined VULKAN_SDK set "VULKAN_SDK=C:\VulkanSDK\%%I"
)
if not defined VULKAN_SDK (
    echo ERROR: A Vulkan SDK is required to build the Cartographer workbench.
    echo        Set VULKAN_SDK to the installed SDK directory.
    exit /b 2
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
    echo ERROR: VULKAN_SDK does not contain Include\vulkan\vulkan.h: %VULKAN_SDK%
    exit /b 2
)
if not exist "%VULKAN_SDK%\Lib\vulkan-1.lib" (
    echo ERROR: VULKAN_SDK does not contain Lib\vulkan-1.lib: %VULKAN_SDK%
    exit /b 2
)
if not exist "%VULKAN_SDK%\Bin\glslc.exe" (
    echo ERROR: VULKAN_SDK does not contain Bin\glslc.exe: %VULKAN_SDK%
    exit /b 2
)

if not defined CARTO_IMGUI_ROOT if defined LOCALAPPDATA if exist "%LOCALAPPDATA%\HiddenCanopy\Dependencies\imgui-v1.92.9\imgui.h" set "CARTO_IMGUI_ROOT=%LOCALAPPDATA%\HiddenCanopy\Dependencies\imgui-v1.92.9"
if not defined CARTO_IMGUI_ROOT (
    for /f "delims=" %%I in ('dir /b /ad /o-n "%TEMP%\cartographer-imgui-*" 2^>nul') do if not defined CARTO_IMGUI_ROOT set "CARTO_IMGUI_ROOT=%TEMP%\%%I"
)
if not defined CARTO_IMGUI_ROOT (
    echo ERROR: A pinned Dear ImGui source tree is required to build the Cartographer workbench.
    echo        Set CARTO_IMGUI_ROOT to a tree containing imgui.h and the Win32/Vulkan backends.
    exit /b 2
)
if not exist "%CARTO_IMGUI_ROOT%\imgui.h" (
    echo ERROR: CARTO_IMGUI_ROOT does not contain imgui.h: %CARTO_IMGUI_ROOT%
    exit /b 2
)
if not exist "%CARTO_IMGUI_ROOT%\backends\imgui_impl_win32.cpp" (
    echo ERROR: CARTO_IMGUI_ROOT does not contain the Win32 backend: %CARTO_IMGUI_ROOT%
    exit /b 2
)
if not exist "%CARTO_IMGUI_ROOT%\backends\imgui_impl_vulkan.cpp" (
    echo ERROR: CARTO_IMGUI_ROOT does not contain the Vulkan backend: %CARTO_IMGUI_ROOT%
    exit /b 2
)

set "PATH=%VULKAN_SDK%\Bin;%PATH%"
set "CARTO_TARGET=cartographer_desktop"
set "CARTO_BUILT_EXE=%CARTO_BUILD_DIR%\apps\desktop\cartographer_desktop.exe"
set "CARTO_DIST_DIR=%CARTO_SOURCE%\dist"
set "CARTO_DIST_EXE=%CARTO_DIST_DIR%\Cartographer.exe"

where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: cmake.exe is not available on PATH.
    exit /b 2
)
where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo ERROR: ninja.exe is not available on PATH.
    exit /b 2
)

set /a CARTO_ATTEMPT=0

:build_attempt
set /a CARTO_ATTEMPT+=1
echo.
echo [Cartographer] Build attempt %CARTO_ATTEMPT% of %CARTO_BUILD_ATTEMPTS%
echo [Cartographer] Source: %CARTO_SOURCE%
echo [Cartographer] Build:  %CARTO_BUILD_DIR%
echo [Cartographer] Vulkan: %VULKAN_SDK%
echo [Cartographer] ImGui:  %CARTO_IMGUI_ROOT%

cmake.exe -S "%CARTO_SOURCE%" -B "%CARTO_BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_EXE_LINKER_FLAGS=-static ^
    -DVulkan_INCLUDE_DIR="%VULKAN_SDK%\Include" ^
    -DVulkan_LIBRARY="%VULKAN_SDK%\Lib\vulkan-1.lib" ^
    -DCARTO_IMGUI_ROOT="%CARTO_IMGUI_ROOT%" ^
    -DCARTO_ENABLE_D3D12=OFF ^
    -DCARTO_BUILD_D3D12_DESKTOP=OFF ^
    -DCARTO_ENABLE_VULKAN=ON ^
    -DCARTO_BUILD_DESKTOP=ON ^
    -DCARTO_BUILD_TESTS=OFF ^
    -DCARTO_BUILD_BENCHMARKS=OFF ^
    -DCARTO_BUILD_CLI=OFF ^
    -DCARTO_BUILD_DEVICE_CPU=OFF ^
    -DCARTO_BUILD_SDK_SHARED=OFF ^
    -DCARTO_BUILD_PRIVATE_INTEGRATIONS=OFF
if errorlevel 1 goto build_failed

cmake.exe --build "%CARTO_BUILD_DIR%" --config Release ^
    --target %CARTO_TARGET% --parallel %CARTO_BUILD_JOBS%
if errorlevel 1 goto build_failed

if not exist "%CARTO_BUILT_EXE%" (
    echo ERROR: Build completed without producing %CARTO_BUILT_EXE%
    goto build_failed
)

for %%F in ("%CARTO_BUILT_EXE%") do if %%~zF LEQ 0 (
    echo ERROR: The produced executable is empty.
    goto build_failed
)

if not exist "%CARTO_DIST_DIR%" mkdir "%CARTO_DIST_DIR%"
if errorlevel 1 (
    echo ERROR: Unable to create %CARTO_DIST_DIR%
    exit /b 1
)
copy /Y "%CARTO_BUILT_EXE%" "%CARTO_DIST_EXE%" >nul
if errorlevel 1 (
    echo ERROR: Unable to publish %CARTO_DIST_EXE%
    exit /b 1
)

for %%F in ("%CARTO_DIST_EXE%") do (
    echo.
    echo [Cartographer] SUCCESS
    echo [Cartographer] Target:     full Vulkan/ImGui authoring workbench
    echo [Cartographer] Executable: %%~fF
    echo [Cartographer] Bytes:      %%~zF
)
exit /b 0

:build_failed
if %CARTO_ATTEMPT% GEQ %CARTO_BUILD_ATTEMPTS% (
    echo.
    echo ERROR: Cartographer did not produce an executable after %CARTO_ATTEMPT% attempts.
    exit /b 1
)
echo [Cartographer] Attempt %CARTO_ATTEMPT% failed. Retrying in two seconds...
timeout /t 2 /nobreak >nul
goto build_attempt
