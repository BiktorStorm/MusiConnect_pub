<#
.SYNOPSIS
    Create a Python virtual environment, install dependencies from
    requirements.txt, and activate it.

.DESCRIPTION
    Run from PowerShell. For the activated environment to persist in your
    current PowerShell session, dot-source the script:

        . .\setup.ps1

    Running it normally (.\setup.ps1) still creates the venv and installs
    dependencies, but the activation ends when the script exits.

.NOTES
    If you get an execution policy error, allow scripts for the current
    session with:
        Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#>

$ErrorActionPreference = "Stop"

# Directory of this script.
$ScriptDir    = Split-Path -Parent $MyInvocation.MyCommand.Path
$VenvDir      = Join-Path $ScriptDir ".venv"
$Requirements = Join-Path $ScriptDir "requirements.txt"

# Pick an available Python interpreter.
$Python = $null
if (Get-Command python -ErrorAction SilentlyContinue) {
    $Python = "python"
} elseif (Get-Command py -ErrorAction SilentlyContinue) {
    $Python = "py"
} else {
    Write-Error "Python is not installed or not on PATH."
    return
}

# Create the virtual environment if it doesn't already exist.
if (-not (Test-Path $VenvDir)) {
    Write-Host "Creating virtual environment in $VenvDir..."
    & $Python -m venv $VenvDir
} else {
    Write-Host "Virtual environment already exists at $VenvDir."
}

# Locate the PowerShell activation script.
$Activate = Join-Path $VenvDir "Scripts\Activate.ps1"
if (-not (Test-Path $Activate)) {
    Write-Error "Could not find the activation script at $Activate."
    return
}

# Activate the environment.
Write-Host "Activating virtual environment..."
& $Activate

# Upgrade pip and install dependencies.
Write-Host "Upgrading pip..."
python -m pip install --upgrade pip

if (Test-Path $Requirements) {
    Write-Host "Installing dependencies from $Requirements..."
    python -m pip install -r $Requirements
} else {
    Write-Warning "$Requirements not found; skipping dependency installation."
}

Write-Host ""
Write-Host "Setup complete. Virtual environment is active."
Write-Host "If activation didn't persist, dot-source the script: . .\setup.ps1"
