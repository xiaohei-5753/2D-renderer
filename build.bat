@echo off
cd /d %~dp0

echo Building EasyRenderer Paint App...
echo.

REM Check if dependencies exist, copy if needed
if not exist "include\GL\glew.h" (
    echo Copying dependencies from magic-simulator...
    if exist "..\magic-simulator\include\GL\glew.h" (
        xcopy /Y /Q "..\magic-simulator\include\GL" "include\GL\"
        xcopy /Y /Q "..\magic-simulator\include\GLFW" "include\GLFW\"
        xcopy /Y /Q "..\magic-simulator\lib" "lib\"
    ) else (
        echo Error: Dependencies not found in ..\magic-simulator
        echo Please ensure magic-simulator folder exists with glfw and glew
        pause
        exit /b 1
    )
)

REM Build the paint app
echo Compiling paint_app.exe...
g++ -std=c++17 -o paint_app.exe paint_app.cpp easy_renderer.cpp -Iinclude -Llib lib/glew32s.lib -lglfw3 -lopengl32 -lgdi32 -luser32 -static -O3 -march=native -ffast-math -flto -DNDEBUG -DGLEW_STATIC -s

if %errorlevel%==0 (
    echo.
    echo Build successful! Output: paint_app.exe
    echo.
) else (
    echo.
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo To run the app: paint_app.exe
echo.
echo Controls:
echo   Left Mouse  - Draw
echo   Right Mouse - Erase
echo   1-8       - Select color
echo   B          - Brush tool
echo   L          - Line tool  
echo   R          - Rectangle tool
echo   E          - Eraser
echo   C          - Clear canvas
echo   Ctrl+S     - Screenshot
echo   Ctrl+N     - Reset camera
echo   Arrow Keys - Pan
echo   +/-        - Zoom
echo   ESC       - Exit
echo.
pause