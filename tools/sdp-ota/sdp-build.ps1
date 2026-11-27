# Builds one SDP firmware project on a Windows machine that has ESP-IDF and the OTA signing key.
# SynaptaCode sends this script and the project's source ZIP over Synapta Connect, runs it, and reads the
# single JSON line it prints: {"type":"built",...} with the signed image, or {"type":"build_failed",...}.
# Keep this file ASCII: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
param(
    [Parameter(Mandatory = $true)] [string]$Source,
    [Parameter(Mandatory = $true)] [string]$ProjectId,
    [Parameter(Mandatory = $true)] [string]$BuildId,
    [Parameter(Mandatory = $true)] [string]$Idf,
    [Parameter(Mandatory = $true)] [string]$SigningKey,
    [Parameter(Mandatory = $true)] [string]$WorkRoot
)
$ErrorActionPreference = 'Stop'

function Write-Result($record) {
    [Console]::Out.WriteLine(($record | ConvertTo-Json -Compress -Depth 4))
}

foreach ($id in @($ProjectId, $BuildId)) {
    if ($id -notmatch '^[a-z0-9][a-z0-9_-]{0,95}$') { throw "Invalid SynaptaCode identifier: $id" }
}
foreach ($file in @($Source, $Idf, $SigningKey)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing file on the build machine: $file" }
}

# One source and build folder per project, reused: ESP-IDF then recompiles only the project's own files,
# so a later build takes about a minute instead of five.
$project = Join-Path $WorkRoot $ProjectId
$src = Join-Path $project 'src'
$out = Join-Path $project 'build'
New-Item -ItemType Directory -Force -Path $project | Out-Null
Get-ChildItem -LiteralPath $project -File | Where-Object { $_.LastWriteTime -lt (Get-Date).AddDays(-14) } | Remove-Item -Force
if (Test-Path -LiteralPath $src) { Remove-Item -LiteralPath $src -Recurse -Force }
Expand-Archive -LiteralPath $Source -DestinationPath $src -Force
if (-not (Test-Path -LiteralPath (Join-Path $src 'CMakeLists.txt'))) {
    Write-Result @{ type = 'build_failed'; buildId = $BuildId; errors = @('CMakeLists.txt is missing: the source folder must be an SDP project (create one with: sdpo new FOLDER)'); tail = @() }
    exit 1
}

$defaults = Join-Path $project 'signing.defaults'
Set-Content -LiteralPath $defaults -Encoding ASCII -Value ('CONFIG_SECURE_BOOT_SIGNING_KEY="' + ($SigningKey -replace '\\', '/') + '"')
$log = Join-Path $project ($BuildId + '.log')
$errorLog = Join-Path $project ($BuildId + '.stderr.log')
$arguments = "-C `"$src`" -B `"$out`" -D IDF_TARGET=esp32 -D SDP_PROJECT_ID=$ProjectId -D SDP_BUILD_ID=$BuildId " +
    "-D `"SDKCONFIG_DEFAULTS=sdkconfig.defaults;$defaults`" build"
# Start-Process keeps compiler stderr as text; with "& idf.cmd 2>&1" PowerShell 5.1 turns each line into an error.
$process = Start-Process -FilePath $Idf -ArgumentList $arguments -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $log -RedirectStandardError $errorLog
$image = Join-Path $out 'sdp_ota.bin'
if ($process.ExitCode -eq 0 -and (Test-Path -LiteralPath $image)) {
    $copy = Join-Path $project ($BuildId + '.bin')
    Copy-Item -LiteralPath $image -Destination $copy -Force
    Write-Result @{ type = 'built'; buildId = $BuildId; image = $copy; bytes = (Get-Item -LiteralPath $copy).Length }
    exit 0
}

# Compiler errors name files under the extracted folder (GCC writes C:/... with forward slashes); show them
# relative to the project instead, as main/app.c:15:9: error: ...
$forward = ($src -replace '\\', '/') + '/'
$lines = @(Get-Content -LiteralPath $log, $errorLog -ErrorAction SilentlyContinue) | ForEach-Object { $_.Replace($src + '\', '').Replace($forward, '') }
$errors = @($lines | Where-Object { $_ -match 'error:|undefined reference|CMake Error|FAILED:' } | Select-Object -First 30)
$tail = @($lines | Select-Object -Last 25)
Write-Result @{ type = 'build_failed'; buildId = $BuildId; exitCode = $process.ExitCode; errors = $errors; tail = $tail; log = $log }
exit 1
