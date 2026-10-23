param([string]$Executable = (Join-Path $PSScriptRoot '..\build\coldedit.exe'))
$ErrorActionPreference = 'Stop'
$workspacePath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outputPath = Join-Path $workspacePath 'artifacts'
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$scenarios = @(
    @{Name='smoke'; Options=''; Scale=0; Rebuilds=1},
    @{Name='dpi-100'; Options='--smoke-scale 1'; Scale=1; Rebuilds=1},
    @{Name='dpi-150'; Options='--smoke-scale 1.5'; Scale=1.5; Rebuilds=1},
    @{Name='dpi-200'; Options='--smoke-scale 2'; Scale=2; Rebuilds=1},
    @{Name='dpi-cycle'; Options='--smoke-dpi-cycle'; Scale=1; Rebuilds=4}
)
foreach ($scenario in $scenarios) {
    $testArgs = "--smoke-test $($scenario.Options) --screenshot artifacts/$($scenario.Name).bmp --report artifacts/$($scenario.Name).json"
    $testProcess = Start-Process -FilePath $Executable -ArgumentList $testArgs -WorkingDirectory $workspacePath -WindowStyle Hidden -Wait -PassThru
    if ($testProcess.ExitCode -ne 0) { throw "$($scenario.Name) failed with exit code $($testProcess.ExitCode)." }
    $report = Get-Content -LiteralPath (Join-Path $outputPath "$($scenario.Name).json") -Raw | ConvertFrom-Json
    if (-not $report.ui_input_checks_passed -or -not $report.selection_boundaries_passed -or $report.selection_boundary_checks -ne 4) { throw "$($scenario.Name): input assertions failed." }
    if ($report.dpi_rebuilds -ne $scenario.Rebuilds) { throw "$($scenario.Name): unexpected DPI rebuild count." }
    if ($scenario.Scale -gt 0 -and ($report.ui_scale -ne $scenario.Scale -or $report.font_pixel_size -ne [Math]::Floor(17 * $scenario.Scale))) { throw "$($scenario.Name): incorrect layout/font scaling." }
    Write-Output "$($scenario.Name): PASS (UI $($report.ui_scale)x, font $($report.font_pixel_size)px, $($report.dpi_rebuilds) atlas rebuilds, four selection-edge checks)"
}
