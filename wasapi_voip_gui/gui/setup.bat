@echo off
REM ============================================================
REM  setup.bat - Create a Python virtual environment, install
REM              dependencies from requirements.txt, and activate it.
REM
REM  Usage (Command Prompt):
REM      setup.bat
REM
REM  Note: When run normally, the activated environment stays
REM  active in your current Command Prompt session after the
REM  script finishes (batch files do not spawn a subshell).
REM ============================================================

setlocal enabledelayedexpansion

REM Directory of this script.
set "SCRIPT_DIR=%~dp0"
set "VENV_DIR=%SCRIPT_DIR%.venv"
set "REQUIREMENTS=%SCRIPT_DIR%requirements.txt"

REM Pick an available Python interpreter.
where python >nul 2>&1
if %errorlevel%==0 (
    set "PYTHON=python"
) else (
    where py >nul 2>&1
    if !errorlevel!==0 (
        set "PYTHON=py"
    ) else (
        echo Error: Python is not installed or not on PATH.
        exit /b 1
    )
)

REM Create the virtual environment if it doesn't already exist.
if not exist "%VENV_DIR%" (
    echo Creating virtual environment in %VENV_DIR%...
    %PYTHON% -m venv "%VENV_DIR%"
    if errorlevel 1 (
        echo Error: failed to create the virtual environment.
        exit /b 1
    )
) else (
    echo Virtual environment already exists at %VENV_DIR%.
)

REM Activate the environment (this persists in the current cmd session).
set "ACTIVATE=%VENV_DIR%\Scripts\activate.bat"
if not exist "%ACTIVATE%" (
    echo Error: could not find the activation script at %ACTIVATE%.
    exit /b 1
)

echo Activating virtual environment...
call "%ACTIVATE%"

REM Upgrade pip and install dependencies.
echo Upgrading pip...
python -m pip install --upgrade pip

if exist "%REQUIREMENTS%" (
    echo Installing dependencies from %REQUIREMENTS%...
    python -m pip install -r "%REQUIREMENTS%"
) else (
    echo Warning: %REQUIREMENTS% not found; skipping dependency installation.
)

echo.
echo Setup complete. Virtual environment is active in this session.
echo To reactivate later, run: .venv\Scripts\activate.bat

endlocal & call "%VENV_DIR%\Scripts\activate.bat"
