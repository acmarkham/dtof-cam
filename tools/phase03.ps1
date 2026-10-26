param(
    [ValidateSet('setup', 'build', 'flash', 'ports')][string]$Action = 'build',
    [string]$Port = 'COM12'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$env:ARDUINO_DIRECTORIES_DATA = Join-Path $projectRoot '.tools/arduino/data'
$env:ARDUINO_DIRECTORIES_DOWNLOADS = Join-Path $projectRoot '.tools/arduino/downloads'
$env:ARDUINO_DIRECTORIES_USER = Join-Path $projectRoot '.tools/arduino/user'
$cliCommand = Get-Command arduino-cli -ErrorAction SilentlyContinue
$cli = if ($cliCommand) { $cliCommand.Source } else {
    Join-Path $env:LOCALAPPDATA 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe'
}
if (-not (Test-Path -LiteralPath $cli)) { throw 'Install Arduino CLI or Arduino IDE first.' }
$fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi'
$sketch = Join-Path $projectRoot 'Phase03/Phase03'
$build = Join-Path $projectRoot '.build/Phase03'
$sdFat = Join-Path $projectRoot '.tools/SdFat'
function Invoke-Cli {
    & $cli @args
    if ($LASTEXITCODE -ne 0) { throw "Arduino CLI failed: $args" }
}
switch ($Action) {
    'setup' {
        Invoke-Cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
        Invoke-Cli core install esp32:esp32@3.3.2 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
        # This release is available upstream but is absent from some Arduino indexes.
        if (-not (Test-Path -LiteralPath $sdFat)) {
            rtk git clone --depth 1 --branch 2.3.1 https://github.com/greiman/SdFat.git $sdFat
            if ($LASTEXITCODE -ne 0) { throw 'SdFat download failed' }
        }
        $sdFatCommit = & rtk proxy git -C $sdFat rev-parse HEAD
        if ($sdFatCommit -ne 'cda057318bec196183d4cc92b01bc1dd64bbfb02') {
            throw 'SdFat checkout does not match pinned 2.3.1 commit'
        }
    }
    'ports' { Invoke-Cli board list }
    'build' { Invoke-Cli compile --fqbn $fqbn --warnings all --library $sdFat --build-path $build $sketch }
    'flash' { Invoke-Cli upload --fqbn $fqbn --port $Port --input-dir $build $sketch }
}
# Windows PowerShell can otherwise propagate stderr warnings as the script status.
exit 0


