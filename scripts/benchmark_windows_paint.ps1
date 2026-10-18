param(
    [string]$BuildDirectory = 'build/check',
    [string]$BaselineRef = 'fa04cb45bd1f405e0c6a06073b2be1db539b8661',
    [ValidateRange(1, 30)][int]$Rounds = 5,
    [ValidateRange(1, 10000)][int]$Samples = 160,
    [ValidateRange(0, 10000)][int]$Warmup = 64,
    [int[]]$Dpi = @(96, 144),
    [string[]]$Scenes = @('empty', 'gradients_static', 'gradients_animated', 'gradients_churn',
        'shadows_static', 'shadows_animated', 'shadows_churn', 'mixed'),
    [switch]$CleanCheck,
    [string]$SummarizeDirectory = ''
)

$ErrorActionPreference = 'Stop'
if (!$SummarizeDirectory) {
    $root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $frameworkBuild = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
        [IO.Path]::GetFullPath($BuildDirectory)
    } else {
        [IO.Path]::GetFullPath((Join-Path $root $BuildDirectory))
    }
    $cachePath = Join-Path $frameworkBuild 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cachePath)) { throw "Missing compatible framework build: $frameworkBuild" }
    function CacheValue([string]$key) {
        $line = Get-Content -LiteralPath $cachePath | Where-Object { $_ -match "^${key}:[^=]+=" } | Select-Object -First 1
        if ($line) { return ($line -split '=', 2)[1] }
        return ''
    }
    $generator = CacheValue 'CMAKE_GENERATOR'
    if ($generator -notlike 'Visual Studio*' -or (CacheValue 'HUXERUI_WINDOWS_7_COMPAT') -eq 'ON') {
        throw 'Use the existing ordinary Windows Visual Studio build for this benchmark'
    }
    $baseline = (& git -C $root rev-parse --verify "${BaselineRef}^{commit}").Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the baseline commit' }
    $runId = Get-Date -Format 'yyyyMMdd-HHmmss'
    $output = Join-Path $frameworkBuild "paint-benchmark-runs/$runId"
    New-Item -ItemType Directory -Path $output | Out-Null
    $snapshot = Join-Path $output 'baseline_renderer.cpp'
    $extract = [Diagnostics.ProcessStartInfo]::new('git')
    $extract.UseShellExecute = $false
    $extract.RedirectStandardOutput = $true
    $extract.RedirectStandardError = $true
    foreach ($argument in @('-C', $root, 'show', "${baseline}:platform/windows/win32_renderer.cpp")) {
        $extract.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($extract)
    $stream = [IO.File]::Create($snapshot)
    try { $process.StandardOutput.BaseStream.CopyTo($stream) } finally { $stream.Dispose() }
    $extractError = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $extractExitCode = $process.ExitCode
    $process.Dispose()
    if ($extractExitCode -ne 0) { throw "Cannot extract the original baseline renderer: $extractError" }
    $optimizedSnapshot = Join-Path $output 'optimized_renderer.cpp'
    Copy-Item -LiteralPath (Join-Path $root 'platform/windows/win32_renderer.cpp') -Destination $optimizedSnapshot
    $benchmarkSource = Join-Path $root 'tests/platform/windows/paint_benchmark'
    $benchmarkBuild = Join-Path $frameworkBuild 'paint-benchmark'
    function ConfigureAndBuild([string]$directory) {
        $configure = @('-S', $benchmarkSource, '-B', $directory, '-G', $generator,
            "-DHUXERUI_FRAMEWORK_BUILD=$($frameworkBuild.Replace('\', '/'))",
            "-DHUXERUI_BASELINE_RENDERER=$($snapshot.Replace('\', '/'))",
            "-DHUXERUI_OPTIMIZED_RENDERER=$($optimizedSnapshot.Replace('\', '/'))")
        foreach ($key in @('CMAKE_GENERATOR_INSTANCE', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET')) {
            $value = CacheValue $key
            if ($value) { $configure += "-D${key}=$value" }
        }
        & cmake @configure
        if ($LASTEXITCODE -ne 0) { throw 'Benchmark configure failed' }
        & cmake --build $directory --config Release --parallel 2
        if ($LASTEXITCODE -ne 0) { throw 'Benchmark build failed' }
    }
    ConfigureAndBuild $benchmarkBuild
    if ($CleanCheck) {
        ConfigureAndBuild (Join-Path $frameworkBuild "paint-benchmark-clean-$runId")
        & cmake --build $benchmarkBuild --config Release --parallel 2
        if ($LASTEXITCODE -ne 0) { throw 'Incremental benchmark build failed' }
    }
    $metadata = [ordered]@{
        local_time = (Get-Date -Format o)
        baseline_commit = $baseline
        optimized_head = (& git -C $root rev-parse HEAD).Trim()
        baseline_renderer_sha256 = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash
        optimized_renderer_sha256 = (Get-FileHash -LiteralPath $optimizedSnapshot -Algorithm SHA256).Hash
        harness_sha256 = (Get-FileHash -LiteralPath (Join-Path $benchmarkSource 'benchmark.cpp') -Algorithm SHA256).Hash
        framework_archive_sha256 = (Get-FileHash -LiteralPath (Join-Path $frameworkBuild 'lib/Release/huxerui_static.lib') -Algorithm SHA256).Hash
        git_status = @(& git -C $root status --short)
        cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber
        display_adapters = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
        power_scheme = @(& powercfg /getactivescheme)
        generator = $generator
        generator_instance = (CacheValue 'CMAKE_GENERATOR_INSTANCE')
        configuration = 'Release'
        framework_profiling = (CacheValue 'HUXERUI_ENABLE_PROFILING')
        compiler_flags = @(Get-Content -LiteralPath (Join-Path $benchmarkBuild 'CMakeCache.txt') |
            Where-Object { $_ -match '^CMAKE_CXX_FLAGS(:|_RELEASE:)' })
        compiler = @(Get-ChildItem -Path (Join-Path $benchmarkBuild 'CMakeFiles/*/CMakeCXXCompiler.cmake') |
            Get-Content |
            Where-Object { $_ -match '^set\(CMAKE_CXX_COMPILER(_VERSION)? ' })
        rounds = $Rounds
        samples_per_round = $Samples
        warmup_frames = $Warmup
        dpi = $Dpi
        scenes = $Scenes
        protocol = 'Separate processes; alternating A/B order; offscreen native replay; GPU timestamp queries; readback outside timing'
    }
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'metadata.json') -Encoding utf8

    $rows = [Collections.Generic.List[object]]::new()
    $culture = [Globalization.CultureInfo]::InvariantCulture
    for ($round = 0; $round -lt $Rounds; ++$round) {
        foreach ($dpiValue in $Dpi) {
            foreach ($scene in $Scenes) {
                $order = if ($round % 2 -eq 0) { @('baseline', 'optimized') } else { @('optimized', 'baseline') }
                $checksums = @{}
                $devices = @{}
                foreach ($variant in $order) {
                    $prefix = "${scene}-${dpiValue}-r${round}-${variant}"
                    $raw = Join-Path $output "$prefix.csv"
                    $device = Join-Path $output "$prefix.log"
                    $executable = Join-Path $benchmarkBuild "Release/paint_benchmark_${variant}.exe"
                    & $executable $scene $dpiValue $Warmup $Samples 1> $raw 2> $device
                    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $prefix; see $device" }
                    $nativeRows = @(Import-Csv -LiteralPath $raw)
                    if ($nativeRows.Count -ne $Samples) { throw "Unexpected sample count: $prefix" }
                    $checksums[$variant] = $nativeRows[0].checksum
                    $devices[$variant] = (Get-Content -LiteralPath $device | Select-Object -First 1)
                    foreach ($native in $nativeRows) {
                        $rows.Add([pscustomobject]@{
                            scene = $scene; dpi = $dpiValue; round = $round; variant = $variant
                            sample = [int]$native.sample
                            submit_us = [double]::Parse($native.submit_us, $culture)
                            gpu_us = [double]::Parse($native.gpu_us, $culture)
                            completed_us = [double]::Parse($native.completed_us, $culture)
                            process_cpu_us = [double]::Parse($native.process_cpu_us, $culture)
                            private_before = [long]$native.private_before
                            private_after = [long]$native.private_after
                            working_set = [long]$native.working_set
                            checksum = $native.checksum; rejected = [int]$native.rejected
                        })
                    }
                }
                if ($checksums.baseline -ne $checksums.optimized) { throw "Pixel mismatch: $scene, DPI $dpiValue, round $round" }
                if ($devices.baseline -ne $devices.optimized) { throw "Adapter mismatch: $scene, DPI $dpiValue, round $round" }
                Write-Host "Round $($round + 1)/$Rounds, DPI $dpiValue, ${scene}: pixels match"
            }
        }
    }
    $rows | Export-Csv -LiteralPath (Join-Path $output 'samples.csv') -NoTypeInformation -Encoding utf8
} else {
    $output = (Resolve-Path -LiteralPath $SummarizeDirectory).Path
    $savedMetadata = Get-Content -LiteralPath (Join-Path $output 'metadata.json') -Raw | ConvertFrom-Json
    $Scenes = @($savedMetadata.scenes)
    $Dpi = @($savedMetadata.dpi)
    $rows = @(Import-Csv -LiteralPath (Join-Path $output 'samples.csv'))
}
function Percentile($values, [double]$fraction) {
    $ordered = @($values | ForEach-Object { [double]::Parse("$_", [Globalization.CultureInfo]::InvariantCulture) } | Sort-Object)
    return $ordered[[Math]::Max(0, [int][Math]::Ceiling($ordered.Count * $fraction) - 1)]
}
$roundStatistics = foreach ($group in ($rows | Group-Object scene, dpi, round, variant)) {
    $data = @($group.Group)
    [pscustomobject]@{
        scene = $data[0].scene; dpi = $data[0].dpi; round = $data[0].round; variant = $data[0].variant
        submit_median_us = Percentile $data.submit_us 0.5
        submit_p95_us = Percentile $data.submit_us 0.95
        gpu_median_us = Percentile $data.gpu_us 0.5
        process_cpu_us = $data[0].process_cpu_us
        private_mib = [double]$data[0].private_after / 1MB
        private_growth_mib = ([double]$data[0].private_after - [double]$data[0].private_before) / 1MB
        rejected = $data[0].rejected
    }
}
$roundStatistics | Sort-Object scene, dpi, round, variant |
    Export-Csv -LiteralPath (Join-Path $output 'rounds.csv') -NoTypeInformation -Encoding utf8
$summary = foreach ($scene in $Scenes) {
    foreach ($dpiValue in $Dpi) {
        $before = @($rows | Where-Object { $_.scene -eq $scene -and $_.dpi -eq $dpiValue -and $_.variant -eq 'baseline' })
        $after = @($rows | Where-Object { $_.scene -eq $scene -and $_.dpi -eq $dpiValue -and $_.variant -eq 'optimized' })
        $beforeSubmit = Percentile $before.submit_us 0.5
        $afterSubmit = Percentile $after.submit_us 0.5
        $pairedChanges = foreach ($round in @($before.round | Sort-Object -Unique)) {
            $beforeRound = @($before | Where-Object { $_.round -eq $round })
            $afterRound = @($after | Where-Object { $_.round -eq $round })
            ((Percentile $afterRound.submit_us 0.5) / (Percentile $beforeRound.submit_us 0.5) - 1) * 100
        }
        [pscustomobject]@{
            scene = $scene; dpi = $dpiValue; samples_per_variant = $before.Count
            baseline_submit_median_us = $beforeSubmit
            optimized_submit_median_us = $afterSubmit
            submit_change_percent = [Math]::Round(($afterSubmit / $beforeSubmit - 1) * 100, 2)
            baseline_submit_p95_us = Percentile $before.submit_us 0.95
            optimized_submit_p95_us = Percentile $after.submit_us 0.95
            paired_change_min_percent = Percentile $pairedChanges 0
            paired_change_max_percent = Percentile $pairedChanges 1
            baseline_process_cpu_us = Percentile $before.process_cpu_us 0.5
            optimized_process_cpu_us = Percentile $after.process_cpu_us 0.5
            baseline_gpu_median_us = Percentile $before.gpu_us 0.5
            optimized_gpu_median_us = Percentile $after.gpu_us 0.5
            baseline_gpu_p95_us = Percentile $before.gpu_us 0.95
            optimized_gpu_p95_us = Percentile $after.gpu_us 0.95
            baseline_private_mib = (Percentile $before.private_after 0.5) / 1MB
            optimized_private_mib = (Percentile $after.private_after 0.5) / 1MB
            optimized_private_growth_mib = (Percentile ($after | ForEach-Object { [double]$_.private_after - [double]$_.private_before }) 0.5) / 1MB
        }
    }
}
$summary | Export-Csv -LiteralPath (Join-Path $output 'summary.csv') -NoTypeInformation -Encoding utf8
$summary | Format-Table scene, dpi, baseline_submit_median_us, optimized_submit_median_us, submit_change_percent -AutoSize
Write-Host "Results: $output"
