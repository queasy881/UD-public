# install.ps1 -- bootstrap installer for UD on Windows.
#
# Curl-fetchable (PowerShell):
#   irm https://raw.githubusercontent.com/queasy881/UD-public/main/install.ps1 | iex
#
# Builds `ud.exe`, adds it to your user PATH, associates .ud files (per-user, no
# admin needed), installs the VS Code extension, and runs a self-check.

$ErrorActionPreference = "Stop"

$Repo   = "https://github.com/queasy881/UD-public.git"
$Prefix = if ($env:UD_PREFIX) { $env:UD_PREFIX } else { Join-Path $env:USERPROFILE ".ud" }
$BinDir = Join-Path $Prefix "bin"

function Say  ($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn ($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die  ($m) { Write-Host "xx  $m" -ForegroundColor Red; exit 1 }

# --- 1. locate or fetch the source ------------------------------------------
if ((Test-Path "main.c") -and (Test-Path "vm.c") -and (Test-Path "build.bat")) {
    $Src = (Get-Location).Path
    Say "Building from the current directory: $Src"
} else {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die "git is required to fetch UD." }
    $Src = Join-Path $Prefix "src"
    Say "Cloning UD into $Src"
    if (Test-Path $Src) { Remove-Item -Recurse -Force $Src }
    New-Item -ItemType Directory -Force -Path $Prefix | Out-Null
    git clone --depth 1 $Repo $Src
}

# --- 2. build ----------------------------------------------------------------
$cc = if ($env:CC) { $env:CC } else { "gcc" }
if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
    Die "a C compiler ('$cc') was not found on PATH. Install MSYS2/MinGW-w64 or LLVM and re-run."
}
Say "Compiling the interpreter"
Push-Location $Src
$srcs = (Get-ChildItem *.c).Name
$gccArgs = @(
    "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-o", "ud.exe"
) + $srcs + @("-s", "-Wl,--gc-sections")
& $cc @gccArgs
if ($LASTEXITCODE -ne 0) { Pop-Location; Die "the build failed." }
Pop-Location

# --- 3. install the binary ---------------------------------------------------
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
Copy-Item -Force (Join-Path $Src "ud.exe") (Join-Path $BinDir "ud.exe")
Say "Installed $BinDir\ud.exe"

# --- 4. put it on the user PATH ---------------------------------------------
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($null -eq $userPath) { $userPath = "" }
if (($userPath -split ";") -notcontains $BinDir) {
    $newPath = if ($userPath.TrimEnd(";") -eq "") { $BinDir } else { $userPath.TrimEnd(";") + ";" + $BinDir }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    $env:Path = "$env:Path;$BinDir"
    Say "Added $BinDir to your user PATH"
}

# --- 5. associate .ud files (per-user registry, no admin) -------------------
$exe = Join-Path $BinDir "ud.exe"
New-Item -Path "HKCU:\Software\Classes\.ud" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\.ud" -Name "(default)" -Value "UD.Script"
New-Item -Path "HKCU:\Software\Classes\UD.Script\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\UD.Script" -Name "(default)" -Value "UD source file"
Set-ItemProperty -Path "HKCU:\Software\Classes\UD.Script\shell\open\command" -Name "(default)" -Value "`"$exe`" `"%1`" %*"
Say "Associated .ud files with ud.exe"

# --- 6. install the VS Code extension ----------------------------------------
$vsSrc = Join-Path $Src "vscode-ud"
if (Test-Path $vsSrc) {
    foreach ($root in @("$env:USERPROFILE\.vscode\extensions", "$env:USERPROFILE\.vscode-insiders\extensions")) {
        $base = Split-Path $root -Parent
        if (-not (Test-Path $base)) { continue }
        $dest = Join-Path $root "ud-lang"
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Copy-Item -Recurse -Force (Join-Path $vsSrc "*") $dest
        Say "Installed the VS Code extension into $dest"
    }
}

# --- 7. self-check -----------------------------------------------------------
Say "Self-check:"
& $exe --version
if ($LASTEXITCODE -ne 0) { Die "the installed binary did not run." }

Write-Host ""
Write-Host "UD is installed." -ForegroundColor Green
Write-Host "  Binary : $exe"
Write-Host "  Try    : ud `"$Src\examples\hello.ud`""
Write-Host "Open a new terminal so the updated PATH takes effect."
