param(
    [string]$Binary = "build\Release\zerotick.exe",
    [string]$Config = "config.json",
    [string]$Output = "data\backtest\backtest_200_sessions.jsonl",
    [int]$TrainingSessions = 100,
    [int]$ValidationSessions = 100,
    [switch]$ForceLogin,
    [switch]$Overwrite
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$binaryPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Binary))
$configPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Config))
$outputPath = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Output))

if ($TrainingSessions -le 0 -or $ValidationSessions -le 0) {
    throw "TrainingSessions and ValidationSessions must both be positive."
}
if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
    throw "Binary not found: $binaryPath. Build the Release target first."
}
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    throw "Configuration not found: $configPath"
}
if (Test-Path -LiteralPath $outputPath) {
    if (-not $Overwrite) {
        throw "Output already exists: $outputPath. Choose another path or use -Overwrite."
    }
    Remove-Item -LiteralPath $outputPath
}
$partialPath = "$outputPath.partial"
if (Test-Path -LiteralPath $partialPath) {
    throw "An incomplete collection exists: $partialPath. Inspect or remove it first."
}

$outputDirectory = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$temporaryConfig = Join-Path ([System.IO.Path]::GetTempPath()) (
    "zerotick-backtest-export-{0}.json" -f [guid]::NewGuid().ToString("N"))

try {
    $settings = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $settings.run.mode = "export_backtest_data"
    $settings.run.force_login = [bool]$ForceLogin
    $settings.run.data_output_path = $outputPath
    if ($null -eq $settings.run.PSObject.Properties["historical_training_sessions"]) {
        $settings.run | Add-Member -NotePropertyName historical_training_sessions -NotePropertyValue $TrainingSessions
    } else {
        $settings.run.historical_training_sessions = $TrainingSessions
    }
    if ($null -eq $settings.run.PSObject.Properties["historical_validation_sessions"]) {
        $settings.run | Add-Member -NotePropertyName historical_validation_sessions -NotePropertyValue $ValidationSessions
    } else {
        $settings.run.historical_validation_sessions = $ValidationSessions
    }
    $settings | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $temporaryConfig -Encoding UTF8

    Write-Host "Collecting $TrainingSessions training sessions and $ValidationSessions validation sessions..."
    & $binaryPath --config $temporaryConfig
    if ($LASTEXITCODE -ne 0) {
        throw "ZeroTick collector exited with code $LASTEXITCODE."
    }
    Write-Host "Dataset written to $outputPath"
} finally {
    if (Test-Path -LiteralPath $temporaryConfig) {
        Remove-Item -LiteralPath $temporaryConfig
    }
}
