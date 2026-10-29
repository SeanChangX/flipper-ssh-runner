<#
.SYNOPSIS
    Local gate for SSH Runner (flipper-ssh-runner). Green here == the branch may merge.

.DESCRIPTION
    Steps (each can be skipped for a quick iteration, but a merge needs all of them):
      1. proto mirror     - flipper-app/lib/proto matches proto/
      2. c format         - the C code is clang-formatted (tools/format_c.sh --check, in the IDF container)
      3. host tests       - proto/, flipper-app/tests/, firmware-esp32/test_host/ via ctest in the pinned IDF container
      4. tools tests      - pytest for tools/ (offline; never touches a Flipper)
      5. cli              - the PC tool (cli/, Go): go mod tidy -diff, go vet, go test and a build
      6. fap              - SSH Runner built with uFBT against every SDK in flipper-app/SDK_VERSION
      7. firmware         - ESP-IDF firmware built in the pinned container (firmware-esp32/IDF_VERSION)
      8. firmware-efusetest - same with the virtual-eFuse test overlay (sdkconfig.efusetest)

    Footprint: everything generated stays in venv\ or .cache\ inside the repo
    (uFBT SDKs, builds, ccache, pip, pytest, pycache). Docker runs with --rm, bind-mounts
    only this folder and uses no named volumes. Docker Desktop must already be running;
    this script never starts it. .github/workflows/check.yml mirrors these steps; it runs
    c-format and host-tests on the plain runner instead of the IDF container, and caches
    ccache for the firmware builds.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\check.ps1
    powershell -ExecutionPolicy Bypass -File tools\check.ps1 -Skip firmware,firmware-efusetest
#>
[CmdletBinding()]
param(
    [ValidateSet("proto-mirror", "c-format", "host-tests", "tools-tests", "cli", "fap", "firmware", "firmware-efusetest")]
    [string[]]$Skip = @()
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Cache = Join-Path $Root ".cache"
$Py = Join-Path $Root "venv\Scripts\python.exe"
$Ufbt = Join-Path $Root "venv\Scripts\ufbt.exe"
$env:PYTHONUTF8 = "1"
$env:PYTHONDONTWRITEBYTECODE = "1"
$env:PYTHONPYCACHEPREFIX = Join-Path $Cache "pycache"
$env:PIP_CACHE_DIR = Join-Path $Cache "pip"

$results = [ordered]@{}

function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments, [string]$WorkDir = $Root)
    Push-Location $WorkDir
    try {
        & $Exe @Arguments | Out-Host
        return $LASTEXITCODE
    } finally {
        Pop-Location
    }
}

function Step {
    param([string]$Name, [scriptblock]$Body)
    if ($Skip -contains $Name) {
        $results[$Name] = "SKIPPED"
        Write-Host "== $Name : skipped" -ForegroundColor Yellow
        return
    }
    Write-Host "== $Name" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $ok = $false
    try { $ok = & $Body } catch { Write-Host $_ -ForegroundColor Red; $ok = $false }
    $results[$Name] = if ($ok) { "PASS ({0:n0} s)" -f $sw.Elapsed.TotalSeconds } else { "FAIL" }
}

function Read-KeyValueFile([string]$Path) {
    $map = @{}
    foreach ($line in Get-Content $Path) {
        if ($line -match '^\s*([A-Z_]+)=(.*)$') { $map[$Matches[1]] = $Matches[2].Trim() }
    }
    return $map
}

function Assert-Docker {
    $null = docker info --format "{{.ServerVersion}}" 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Docker is not reachable. Start Docker Desktop, then re-run."
    }
}

# docker run --rm with only this folder mounted; caches and build output under .cache/
function Invoke-Idf {
    param([string]$WorkDir, [string[]]$Command)
    $docker = @("run", "--rm", "-v", "${Root}:/project", "-w", $WorkDir,
        "-e", "CCACHE_DIR=/project/.cache/ccache", "-e", "IDF_CCACHE_ENABLE=1",
        "-e", "IDF_COMPONENT_MANAGER=0", "-e", "PYTHONDONTWRITEBYTECODE=1", $IdfImage) + $Command
    return Invoke-Native "docker" $docker
}

if (-not (Test-Path $Py)) {
    throw "venv missing. Create it first: python -m venv venv; venv\Scripts\pip install --cache-dir .cache\pip -r requirements.txt"
}

# Keep __pycache__ out of the source tree for every use of this venv, not just this script.
$pth = Join-Path $Root "venv\Lib\site-packages\fes_pycache.pth"
$pthLine = "import sys; sys.pycache_prefix = r'" + (Join-Path $Cache "pycache") + "'"
if (-not (Test-Path $pth) -or (Get-Content $pth -Raw).Trim() -ne $pthLine) {
    Set-Content -Path $pth -Value $pthLine -Encoding ascii
}
New-Item -ItemType Directory -Force (Join-Path $Cache "build") | Out-Null

$idf = Read-KeyValueFile (Join-Path $Root "firmware-esp32\IDF_VERSION")
$IdfImage = $idf["IDF_IMAGE"]

Step "proto-mirror" {
    (Invoke-Native $Py @("tools\sync_proto.py", "--check")) -eq 0
}

Step "c-format" {
    Assert-Docker
    (Invoke-Idf "/project" @("bash", "tools/format_c.sh", "--check")) -eq 0
}

Step "host-tests" {
    Assert-Docker
    (Invoke-Idf "/project" @("bash", "tools/host_tests.sh")) -eq 0
}

Step "tools-tests" {
    if (-not (Test-Path (Join-Path $Root "tools\tests"))) { Write-Host "no tools tests yet"; return $true }
    (Invoke-Native $Py @("-m", "pytest", "-q", "tools\tests")) -eq 0
}

# the PC config tool (cli\, Go): its module and build caches stay in .cache\go
Step "cli" {
    if (-not (Get-Command go -ErrorAction SilentlyContinue)) {
        # no Go on this PC: the pinned image of the version cli\go.mod names, caches in .cache\go
        Assert-Docker
        $ver = (Select-String -Path (Join-Path $Root "cli\go.mod") -Pattern '^go (\S+)').Matches[0].Groups[1].Value
        $docker = @("run", "--rm", "-v", "${Root}:/project", "-w", "/project/cli",
            "-e", "GOPATH=/project/.cache/go", "-e", "GOMODCACHE=/project/.cache/go/mod",
            "-e", "GOCACHE=/project/.cache/go/build", "-e", "GOFLAGS=-modcacherw", "-e", "GOTOOLCHAIN=local",
            "-e", "CGO_ENABLED=0", "golang:$ver", "sh", "-c",
            "go mod tidy -diff && go vet ./... && go test ./... && go build -buildvcs=false -o /dev/null ./cmd/sshrunner")  # the version is in ldflags
        return (Invoke-Native "docker" $docker) -eq 0
    }
    $env:GOPATH = Join-Path $Cache "go"; $env:GOMODCACHE = Join-Path $Cache "go\mod"; $env:GOCACHE = Join-Path $Cache "go\build"
    $env:GOFLAGS = "-modcacherw"; $env:GOTOOLCHAIN = "local"; $env:CGO_ENABLED = "0"
    Push-Location (Join-Path $Root "cli")
    try {
        ((Invoke-Native "go" @("mod", "tidy", "-diff")) -eq 0) -and
            ((Invoke-Native "go" @("vet", "./...")) -eq 0) -and ((Invoke-Native "go" @("test", "./...")) -eq 0) -and
            ((Invoke-Native "go" @("build", "-o", "NUL", "./cmd/sshrunner")) -eq 0)
    } finally { Pop-Location }
}

Step "fap" {
    $sdk = Read-KeyValueFile (Join-Path $Root "flipper-app\SDK_VERSION")
    $all = $true
    foreach ($name in @("UNLEASHED", "OFFICIAL")) {
        $url = $sdk["${name}_SDK_URL"]
        $tag = $sdk["${name}_TAG"]
        $home_ = Join-Path $Cache ("ufbt\windows\" + $name.ToLower())
        $env:UFBT_HOME = $home_
        $state = Join-Path $home_ "current\ufbt_state.json"
        $have = if (Test-Path $state) { (Get-Content $state -Raw | ConvertFrom-Json).url } else { "" }
        if ($have -ne $url) {
            Write-Host "-- fetching $name SDK $tag"
            if ((Invoke-Native $Ufbt @("update", "--hw-target=f7", "--url=$url")) -ne 0) { $all = $false; continue }
        }
        # release (talks to the board) and demo (mock backend, no UART code): application.fam
        foreach ($flavour in @("release", "demo")) {
            Write-Host "-- building $flavour against $name $tag"
            $env:SSH_RUNNER_FLAVOUR = $flavour
            # `faps` = build + API check, without writing .vscode/compile_commands.json into the app
            $rc = Invoke-Native $Ufbt @("faps") (Join-Path $Root "flipper-app")
            if ($rc -ne 0) { $all = $false; continue }
            # dist/<sdk>/ssh_runner.fap is the one to install; ssh_runner_demo.fap is for
            # screenshots and development (QUICKSTART)
            $file = if ($flavour -eq "demo") { "ssh_runner_demo.fap" } else { "ssh_runner.fap" }
            $out = Join-Path $Root ("flipper-app\dist\" + $name.ToLower())
            New-Item -ItemType Directory -Force $out | Out-Null
            Copy-Item (Join-Path $Root "flipper-app\dist\ssh_runner.fap") (Join-Path $out $file) -Force
        }
    }
    Remove-Item Env:UFBT_HOME
    Remove-Item Env:SSH_RUNNER_FLAVOUR -ErrorAction SilentlyContinue
    $all
}

Step "firmware" {
    Assert-Docker
    (Invoke-Idf "/project/firmware-esp32" @("idf.py", "-B", "/project/.cache/build/esp32",
            "-D", "SDKCONFIG=/project/.cache/build/esp32/sdkconfig", "build")) -eq 0
}

Step "firmware-efusetest" {
    Assert-Docker
    (Invoke-Idf "/project/firmware-esp32" @("idf.py", "-B", "/project/.cache/build/esp32-efusetest",
            "-D", "SDKCONFIG=/project/.cache/build/esp32-efusetest/sdkconfig",
            "-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.efusetest", "build")) -eq 0
}

Write-Host ""
Write-Host "== summary" -ForegroundColor Cyan
$failed = $false
foreach ($k in $results.Keys) {
    $v = $results[$k]
    $color = if ($v -like "PASS*") { "Green" } elseif ($v -eq "SKIPPED") { "Yellow" } else { $failed = $true; "Red" }
    Write-Host ("  {0,-17} {1}" -f $k, $v) -ForegroundColor $color
}
if ($failed) { Write-Host "check.ps1: FAILED" -ForegroundColor Red; exit 1 }
if ($Skip.Count) { Write-Host "check.ps1: passed with skipped steps (not a merge gate)" -ForegroundColor Yellow; exit 0 }
Write-Host "check.ps1: ALL GREEN" -ForegroundColor Green
exit 0
