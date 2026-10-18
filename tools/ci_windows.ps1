[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $repoRoot
$localRoot = Join-Path $repoRoot '.local'
$ciLogs = Join-Path $localRoot 'ci-logs'
$qtRoot = Join-Path $localRoot 'Qt\6.7.3\msvc2019_64'
$buildRoot = Join-Path $localRoot 'build-candidate'
$ctest = (Get-Command 'ctest.exe' -ErrorAction Stop).Source
$python = (Get-Command 'python.exe' -ErrorAction Stop).Source
$pwsh = (Get-Command 'pwsh.exe' -ErrorAction Stop).Source

New-Item -ItemType Directory -Force -Path $ciLogs | Out-Null

function Invoke-LoggedCommand {
    param(
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][string] $Executable,
        [Parameter(Mandatory = $true)][string[]] $Arguments
    )

    $logPath = Join-Path $ciLogs "$Name.log"
    Write-Host "执行：$Executable $($Arguments -join ' ')"
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "$Name 失败，退出码 $code；日志：$logPath"
    }
}

$env:PYTHONUTF8 = '1'
Invoke-LoggedCommand -Name 'repository-integrity' -Executable $python -Arguments @('tools/verify_repository.py')
Invoke-LoggedCommand -Name 'python-tests' -Executable $python -Arguments @(
    '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py', '-v'
)

# 获取仓库辅助工具支持的全部固定依赖。未列入工具的 type_safe、simdutf
# 和 Uchardet 等仍由 CPM 在配置阶段获取，不使用本机已有的依赖缓存。
$dependencyNames = @(
    'CRoaring', 'streamvbyte', 'maddy', 'KF5Archive', 'robin_hood', 'Catch2',
    'backward-cpp', 'KDSingleApplication', 'xxHash', 'whereami', 'exprtk',
    'KDToolBox', 'efsw', 'tbb', 'mimalloc'
)
$dependencyArguments = @('tools/fetch_dependencies.py', 'fetch')
foreach ($dependencyName in $dependencyNames) {
    $dependencyArguments += @('--name', $dependencyName)
}
Invoke-LoggedCommand -Name 'pinned-dependencies' -Executable $python -Arguments $dependencyArguments

if (-not (Test-Path -LiteralPath (Join-Path $qtRoot 'lib\cmake\Qt6\Qt6Config.cmake') -PathType Leaf)) {
    throw "未在 $qtRoot 找到 Qt 6.7.3 MSVC x64"
}

Invoke-LoggedCommand -Name 'candidate-build' -Executable $pwsh -Arguments @(
    '-NoProfile', '-File', (Join-Path $PSScriptRoot 'build.ps1'),
    '-Variant', 'candidate', '-Backend', 'qt', '-CleanFirst', '-Jobs', '4', '-QtRoot', $qtRoot
)

$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = Join-Path $qtRoot 'plugins'
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $qtRoot 'plugins\platforms'
$env:PATH = "$(Join-Path $qtRoot 'bin');$(Join-Path $buildRoot 'output');$(Join-Path $buildRoot 'reliability-probe');$env:PATH"

$testListPath = Join-Path $ciLogs 'ctest-list.json'
$testList = & $ctest '--test-dir' $buildRoot '--show-only=json-v1' 2>&1
$testListExitCode = $LASTEXITCODE
$testList | Set-Content -LiteralPath $testListPath -Encoding utf8
if ($testListExitCode -ne 0) {
    throw "无法枚举 CTest 测试（退出码 $testListExitCode）；日志：$testListPath"
}
$testDocument = ($testList -join "`n") | ConvertFrom-Json
$actualTests = @($testDocument.tests | ForEach-Object { $_.name } | Sort-Object)
$expectedTests = @(
    'klogg_crawler_tests', 'klogg_itests', 'klogg_literal_tests',
    'klogg_preview_tests', 'klogg_smoke', 'klogg_tests', 'klogg_efsw_lifecycle_tests'
) | Sort-Object
if (($actualTests -join "`n") -ne ($expectedTests -join "`n")) {
    throw "要求七项原生测试（$($expectedTests -join ', ')），实际为（$($actualTests -join ', ')）；日志：$testListPath"
}

Invoke-LoggedCommand -Name 'native-ctest' -Executable $ctest -Arguments @(
    '--test-dir', $buildRoot, '--output-on-failure', '--timeout', '120'
)
# 曾在整套集成测试的对象回收阶段崩溃。任一轮失败即失败，不重试掩盖。
Invoke-LoggedCommand -Name 'native-integration-repeat' -Executable $ctest -Arguments @(
    '--test-dir', $buildRoot, '--output-on-failure', '--timeout', '120',
    '-R', '^klogg_itests$', '--repeat', 'until-fail:10'
)
