# install.ps1 -- bootstrap installer for UD on Windows.
#
# Curl-fetchable (PowerShell):
#   irm https://raw.githubusercontent.com/queasy881/UD-public/main/install.ps1 | iex
#
# Builds `ud.exe`, adds it to your user PATH, associates .ud files (per-user, no
# admin needed), installs the VS Code extension, and runs a self-check.

$ErrorActionPreference = "Stop"

$Repo     = "https://github.com/queasy881/UD-public.git"
$Prefix   = if ($env:UD_PREFIX) { $env:UD_PREFIX } else { Join-Path $env:USERPROFILE ".ud" }
$BinDir   = Join-Path $Prefix "bin"
$AssetDir = Join-Path $Prefix "assets"

function Say  ($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn ($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die  ($m) { Write-Host "xx  $m" -ForegroundColor Red; exit 1 }

# --- 1. locate or fetch the source ------------------------------------------
if ((Test-Path "src/main.c") -and (Test-Path "src/vm.c") -and (Test-Path "build.bat")) {
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
$srcDir = Join-Path $Src "src"
$srcs = (Get-ChildItem (Join-Path $srcDir "*.c")).FullName
$gccArgs = @(
    "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
    "-I", $srcDir,
    "-o", "ud.exe"
) + $srcs + @("-s", "-Wl,--gc-sections")
& $cc @gccArgs
if ($LASTEXITCODE -ne 0) { Pop-Location; Die "the build failed." }
Pop-Location

# --- 3. install the binary and icons -----------------------------------------
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
Copy-Item -Force (Join-Path $Src "ud.exe") (Join-Path $BinDir "ud.exe")
Say "Installed $BinDir\ud.exe"

New-Item -ItemType Directory -Force -Path $AssetDir | Out-Null
foreach ($ico in @("ud-source.ico", "ud-bytecode.ico")) {
    $from = Join-Path $Src "assets\$ico"
    if (Test-Path $from) { Copy-Item -Force $from (Join-Path $AssetDir $ico) }
}

# --- 4. put it on the user PATH ---------------------------------------------
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($null -eq $userPath) { $userPath = "" }
if (($userPath -split ";") -notcontains $BinDir) {
    $newPath = if ($userPath.TrimEnd(";") -eq "") { $BinDir } else { $userPath.TrimEnd(";") + ";" + $BinDir }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    $env:Path = "$env:Path;$BinDir"
    Say "Added $BinDir to your user PATH"
}

# --- 5. associate .ud and .ldx files (per-user registry, no admin) ----------
$exe          = Join-Path $BinDir "ud.exe"
$IconSource   = Join-Path $AssetDir "ud-source.ico"
$IconBytecode = Join-Path $AssetDir "ud-bytecode.ico"

function Register-UDType($ext, $progid, $desc, $command, $icon) {
    New-Item -Path "HKCU:\Software\Classes\$ext" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$ext" -Name "(default)" -Value $progid
    New-Item -Path "HKCU:\Software\Classes\$progid" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progid" -Name "(default)" -Value $desc
    New-Item -Path "HKCU:\Software\Classes\$progid\DefaultIcon" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progid\DefaultIcon" -Name "(default)" -Value $icon
    New-Item -Path "HKCU:\Software\Classes\$progid\shell\open\command" -Force | Out-Null
    Set-ItemProperty -Path "HKCU:\Software\Classes\$progid\shell\open\command" -Name "(default)" -Value $command
}

# .ud runs the source; .ldx runs the compiled program (works for thin + standalone).
Register-UDType ".ud"  "UD.Script"   "UD source file"      "`"$exe`" `"%1`" %*"      $IconSource
Register-UDType ".ldx" "UD.Bytecode" "UD compiled program" "`"$exe`" run `"%1`" %*"  $IconBytecode
Say "Associated .ud and .ldx files (with icons)"

# nudge Explorer to pick up the new icons
try { & "$env:SystemRoot\System32\ie4uinit.exe" -show 2>$null } catch {}

# --- 6. install the VS Code extension ----------------------------------------
$vsSrc = Join-Path $Src "editor\vscode"
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
