param(
    [string]$QtRoot = $env:QT_ROOT_DIR,
    [string]$BuildRoot = "$PSScriptRoot/../build/windows-ci"
)
$ErrorActionPreference = "Stop"
Write-Host "Starting Windows build (PowerShell $($PSVersionTable.PSVersion))"

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    Write-Host "Running: $File $($Arguments -join ' ')"
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed with exit code $LASTEXITCODE" }
}

if (-not $QtRoot) { throw "QT_ROOT_DIR is required" }
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$BuildRoot = (New-Item -ItemType Directory -Force $BuildRoot).FullName
Start-Transcript -Path "$BuildRoot/build.log" -Force | Out-Null
$deps = New-Item -ItemType Directory -Force "$BuildRoot/dependencies"
$prefix = New-Item -ItemType Directory -Force "$deps/prefix"

function Get-Source([string]$Name, [string]$Url, [string]$Sha256) {
    Write-Host "Preparing dependency: $Name"
    $archive = "$deps/$Name"
    if (-not (Test-Path $archive)) {
        Invoke-Checked curl.exe @("--fail", "--location", "--silent", "--show-error",
            "--connect-timeout", "20", "--max-time", "180", "--retry", "2", "--retry-max-time", "300",
            "--output", "$archive.partial", $Url)
        Move-Item "$archive.partial" $archive -Force
    }
    Write-Host "Verifying archive: $Name"
    if ((Get-FileHash $archive -Algorithm SHA256).Hash.ToLower() -ne $Sha256.ToLower()) {
        throw "SHA256 mismatch for $Name"
    }
    $directory = "$deps/$([IO.Path]::GetFileNameWithoutExtension([IO.Path]::GetFileNameWithoutExtension($Name)))"
    if (-not (Test-Path $directory)) {
        Invoke-Checked python @("-c", "import sys, tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter='data')", $archive, "$deps")
    }
    if (-not (Test-Path $directory)) { throw "Source directory is missing: $directory" }
    Write-Host "Dependency ready: $Name"
    return $directory
}

$opus = Get-Source "opus-1.6.1.tar.gz" "https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz" "6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
$samplerate = Get-Source "libsamplerate-0.2.2.tar.xz" "https://github.com/libsndfile/libsamplerate/releases/download/0.2.2/libsamplerate-0.2.2.tar.xz" "3258da280511d24b49d6b08615bbe824d0cacc9842b0e4caf11c52cf2b043893"
$ffmpeg = Get-Source "ffmpeg-7.1.5.tar.xz" "https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz" "de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"
$openssl = Get-Source "openssl-3.6.5.tar.gz" "https://github.com/openssl/openssl/releases/download/openssl-3.6.5/openssl-3.6.5.tar.gz" "a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98"

Push-Location $openssl
Invoke-Checked perl @("Configure", "VC-WIN64A", "shared", "no-tests", "--prefix=$prefix")
Invoke-Checked nmake @()
Invoke-Checked nmake @("install_sw")
Pop-Location

foreach ($pair in @(@($opus, "opus"), @($samplerate, "samplerate"))) {
    $source = $pair[0]; $name = $pair[1]; $dir = "$BuildRoot/$name-build"
    Invoke-Checked cmake @("-S", $source, "-B", $dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON", "-DBUILD_TESTING=OFF", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DLIBSAMPLERATE_EXAMPLES=OFF", "-DOPUS_BUILD_PROGRAMS=OFF", "-DCMAKE_INSTALL_PREFIX=$prefix")
    Invoke-Checked cmake @("--build", $dir, "--parallel", "3")
    Invoke-Checked cmake @("--install", $dir)
}

# The Qt kit supplies the runtime DLLs. Generate only matching MSVC import
# libraries from their exports, then compile against official FFmpeg headers.
$ffmpegBin = "$QtRoot/bin"
foreach ($tool in @("cl.exe", "link.exe", "dumpbin.exe")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "MSVC tool is unavailable: $tool" }
}
$ffmpegStage = New-Item -ItemType Directory -Force "$BuildRoot/ffmpeg-sdk"
New-Item -ItemType Directory -Force "$ffmpegStage/include", "$ffmpegStage/lib" | Out-Null
# Keep the vcvars environment; FFmpeg's mslink wrapper locates link beside cl.
Write-Host "Configuring FFmpeg headers"
& "$env:ProgramFiles/Git/bin/bash.exe" --noprofile --norc -c "cd '$($ffmpeg -replace '\\','/')' && ./configure --toolchain=msvc --disable-programs --disable-doc --disable-autodetect --disable-everything --disable-network --disable-x86asm --disable-debug --disable-postproc --disable-avdevice --disable-avfilter --disable-swresample --disable-avformat --disable-encoders --disable-decoders --disable-muxers --disable-demuxers --disable-parsers --disable-bsfs --disable-protocols --disable-filters --disable-indevs --disable-outdevs --disable-hwaccels"
if ($LASTEXITCODE -ne 0) { throw "FFmpeg header configuration failed" }
foreach ($component in @("libavcodec", "libavutil", "libswscale")) {
    $include = New-Item -ItemType Directory -Force "$ffmpegStage/include/$component"
    Copy-Item "$ffmpeg/$component/*.h" $include -Force
}
foreach ($library in @(@("avcodec-61.dll", "avcodec"), @("avutil-59.dll", "avutil"), @("swscale-8.dll", "swscale"))) {
    $dll = "$ffmpegBin/$($library[0])"; $def = "$BuildRoot/$($library[1]).def"; $lib = "$ffmpegStage/lib/$($library[1]).lib"
    $exports = & dumpbin /exports $dll | Select-String '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    if (-not $exports) { throw "No exports found in $dll" }
    @("LIBRARY $($library[0])", "EXPORTS") + ($exports | ForEach-Object { "$_" }) | Set-Content -Encoding ascii $def
    Invoke-Checked lib @("/def:$def", "/machine:x64", "/out:$lib")
}

$appBuild = "$BuildRoot/app"
$version = @()
if ($env:SQUADSPEAK_VERSION) { $version += "-DSQUADSPEAK_VERSION=$env:SQUADSPEAK_VERSION" }
Invoke-Checked cmake (@("-S", $repo, "-B", $appBuild, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON", "-DCMAKE_PREFIX_PATH=$QtRoot", "-DOPENSSL_ROOT_DIR=$prefix", "-DSQUADSPEAK_AUDIO_DEPS_ROOT=$prefix", "-DSQUADSPEAK_FFMPEG_ROOT=$ffmpegStage") + $version)
Invoke-Checked cmake @("--build", $appBuild, "--parallel", "3")
$env:PATH = "$QtRoot/bin;$prefix/bin;$appBuild/audio-processing/bin;$env:PATH"
$env:QT_FORCE_STDERR_LOGGING = "1"
$env:QT_LOGGING_RULES = ""
Start-Service Audiosrv
Get-CimInstance Win32_SoundDevice | Select-Object Name, Status | Format-Table
$testLog = "$BuildRoot/windows-tests.log"
ctest --test-dir $appBuild --output-on-failure --no-tests=error --output-junit "$BuildRoot/ctest.xml" 2>&1 | Tee-Object $testLog
$testExit = $LASTEXITCODE
$details = "$appBuild/Testing/Temporary/LastTest.log"
if (Test-Path $details) { Copy-Item $details "$BuildRoot/test-details.log" }
if (-not (Test-Path $details)) {
    Write-Warning 'CTest produced no detailed test report'
    $testExit = 1
} elseif (Select-String -Path $details -Pattern '^SKIP\s+:' -Quiet) {
    Write-Warning 'Windows tests skipped required cases'
    $testExit = 1
}

# Gather independent archive evidence even after a failed contract. The final
# test exit status still prevents the release workflow from using this package.
$package = New-Item -ItemType Directory -Force "$BuildRoot/package"
Invoke-Checked cmake @("--install", $appBuild, "--prefix", $package)
if ((Get-FileHash "$repo/LICENSE").Hash -ne (Get-FileHash "$package/LICENSE").Hash) {
    throw "Packaged project license does not match the source"
}
foreach ($notice in Get-ChildItem "$repo/docs/third-party" -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath("$repo/docs/third-party", $notice.FullName)
    if ((Get-FileHash $notice.FullName).Hash -ne (Get-FileHash "$package/THIRD_PARTY_NOTICES/$relative").Hash) {
        throw "Packaged third-party notice does not match the source: $relative"
    }
}
Invoke-Checked "$QtRoot/bin/windeployqt.exe" @("--release", "--qmldir", "$repo/ui", "$package/bin/squadspeak.exe")
Invoke-Checked "$QtRoot/bin/windeployqt.exe" @("--release", "--qmldir", "$repo/ui", "$package/bin/squad_image_worker.exe")
if (-not (Test-Path "$package/bin/sqldrivers/qsqlite.dll")) {
    throw "Packaged Qt SQL SQLite driver is missing: bin/sqldrivers/qsqlite.dll"
}
foreach ($directory in @("$prefix/bin", "$appBuild/audio-processing/bin")) {
    if (Test-Path $directory) {
        $dlls = Get-ChildItem "$directory/*.dll" -ErrorAction Stop
        if (-not $dlls) { throw "No runtime DLLs found in $directory" }
        Copy-Item $dlls.FullName "$package/bin" -Force
    } else {
        throw "Required dependency directory is missing: $directory"
    }
}
foreach ($name in @("avcodec-61.dll", "avformat-61.dll", "avutil-59.dll", "swresample-5.dll", "swscale-8.dll")) {
    if (-not (Test-Path "$QtRoot/bin/$name")) { throw "Qt FFmpeg runtime is missing: $name" }
    Copy-Item "$QtRoot/bin/$name" "$package/bin" -Force
}
foreach ($name in @("squadspeak.exe", "squad_image_worker.exe", "vc_redist.x64.exe", "avcodec-61.dll", "avformat-61.dll", "avutil-59.dll", "swresample-5.dll", "swscale-8.dll")) {
    if (-not (Test-Path "$package/bin/$name")) { throw "Packaged file is missing: $name" }
}
$redistSignature = Get-AuthenticodeSignature "$package/bin/vc_redist.x64.exe"
if ($redistSignature.Status -ne 'Valid' -or $redistSignature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
    throw 'The packaged Visual C++ Redistributable must have a valid Microsoft signature'
}
foreach ($pattern in @("libcrypto-*.dll", "libssl-*.dll", "webrtc-audio-processing-2-1.dll", "opus*.dll", "samplerate*.dll")) {
    if (-not (Get-ChildItem "$package/bin/$pattern" -ErrorAction SilentlyContinue)) {
        throw "Packaged dependency is missing: $pattern"
    }
}
# Preserve Windows-specific copyrights and components from the installed SDK.
# The repository records are from the macOS kit; license texts are shared.
$notices = "$package/THIRD_PARTY_NOTICES"
foreach ($record in Get-ChildItem "$notices/QT-SBOM/*.spdx") {
    $sdkRecord = "$QtRoot/sbom/$($record.Name)"
    if (-not (Test-Path $sdkRecord)) { throw "Qt SDK notice is missing: $sdkRecord" }
    Copy-Item $sdkRecord $record.FullName -Force
}
$qtModules = Get-ChildItem "$package/bin/Qt6*.dll" | ForEach-Object { $_.BaseName.Substring(3) }
Invoke-Checked python (@("$repo/tests/check_dependency_notices.py", $notices, "--qt-modules") + $qtModules)
$archive = "$BuildRoot/squadspeak-windows-x86_64.zip"
Compress-Archive -Path "$package/*" -DestinationPath $archive -Force
$archivePackage = "$BuildRoot/archive-check"
if (Test-Path $archivePackage) { Remove-Item $archivePackage -Recurse -Force }
Expand-Archive -LiteralPath $archive -DestinationPath $archivePackage -Force
$env:PATH = "$archivePackage/bin;$env:SystemRoot\System32;$env:SystemRoot"
Invoke-Checked "$archivePackage/bin/squadspeak.exe" @("--smoke-test", "--settings-file", "$BuildRoot/archive-settings.ini")
# Reuse the subprocess contract against the extracted executable and DLLs.
Copy-Item "$QtRoot/bin/Qt6Test.dll" $appBuild -Force
$env:SQUAD_TEST_APP = "$archivePackage/bin/squadspeak.exe"
try {
    Invoke-Checked "$appBuild/headless_tests.exe" @("productionServerBecomesReadyWithoutGuiOrAudio:argument", "headlessLanguageOptionLoadsCatalogBeforeHelp", "headlessLanguagePrecedenceAndValidation:unicode-diagnostic", "-o", "$BuildRoot/package-headless.log,txt")
} finally {
    Remove-Item Env:SQUAD_TEST_APP
}
Write-Host "Windows package: $BuildRoot/squadspeak-windows-x86_64.zip"
Stop-Transcript | Out-Null
if ($testExit -ne 0) { throw "Windows tests failed with exit code $testExit" }
