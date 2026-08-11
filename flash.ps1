# flash.ps1 — Xiaozhi ESP32-S3-Touch-LCD-1.83 icin guvenli flash
#
# Firmware'i yazar VE hemen ardindan NVS'i geri yukler.
# merged-binary.bin 0x0-0x934FFF arasini siler, NVS (0x9000) tam o araligin icinde;
# ikinci adim atlanirsa WiFi ayarlari ve cihazin xiaozhi.me kimligi kaybolur.
#
# Kullanim:
#   .\flash.ps1                       -> COM8, merged-binary.bin, nvs-only.bin
#   .\flash.ps1 -Port COM5
#   .\flash.ps1 -Firmware stok-turkce.bin
#   .\flash.ps1 -SkipNvs              -> NVS geri yuklemeyi atla (nadiren istenir)

param(
    [string]$Port     = "COM8",
    [string]$Firmware = "merged-binary.bin",
    [string]$Nvs      = "nvs-only.bin",
    [string]$Esptool  = ".\esptool.exe",
    [switch]$SkipNvs
)

$ErrorActionPreference = "Stop"

function Fail($msg) { Write-Host "`n  HATA: $msg" -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host "`n=== $msg ===" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  $msg" -ForegroundColor Green }

# --- On kontroller ---------------------------------------------------------
Step "Kontroller"

if (-not (Test-Path $Esptool)) { Fail "$Esptool bulunamadi. Betigi esptool.exe ile ayni klasorde calistir." }
Ok "esptool bulundu"

if (-not (Test-Path $Firmware)) { Fail "$Firmware bulunamadi. GitHub Actions artifact'ini bu klasore cikart." }
$fwSize = (Get-Item $Firmware).Length
Ok "$Firmware  ($('{0:N0}' -f $fwSize) byte)"

if (-not $SkipNvs) {
    if (-not (Test-Path $Nvs)) {
        Fail "$Nvs bulunamadi. Kritik yedekten uret:`n" +
             "    `$b = [System.IO.File]::ReadAllBytes(`"`$PWD\kritik-yedek.bin`")`n" +
             "    [System.IO.File]::WriteAllBytes(`"`$PWD\nvs-only.bin`", `$b[0x9000..0xCFFF])"
    }
    $nvsSize = (Get-Item $Nvs).Length
    if ($nvsSize -ne 16384) { Fail "$Nvs 16384 byte olmali, $nvsSize byte. Yanlis dosya." }
    Ok "$Nvs  (16384 byte)"
} else {
    Write-Host "  UYARI: NVS geri yukleme ATLANIYOR - WiFi ve cihaz kimligi silinecek." -ForegroundColor Yellow
}

# --- Cihaz bagli mi --------------------------------------------------------
Step "Cihaz kontrolu ($Port)"
& $Esptool -p $Port chip-id | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "$Port uzerinde cihaza ulasilamadi. Kablo veri kablosu mu? Aygit Yoneticisi'nden portu dogrula." }
Ok "ESP32-S3 yanit veriyor"

# --- Onay ------------------------------------------------------------------
Write-Host ""
$ans = Read-Host "Flash'lamaya baslansin mi? (e/h)"
if ($ans -notmatch '^[eEyY]') { Write-Host "Iptal edildi."; exit 0 }

# --- 1) Firmware -----------------------------------------------------------
Step "1/2  Firmware yaziliyor (0x0)"
& $Esptool -p $Port write-flash 0x0 $Firmware
if ($LASTEXITCODE -ne 0) { Fail "Firmware yazilamadi. Cihaz simdi tutarsiz durumda olabilir - NVS adimini calistirmadan once tekrar dene." }
Ok "firmware yazildi"

# --- 2) NVS ----------------------------------------------------------------
if (-not $SkipNvs) {
    Step "2/2  NVS geri yukleniyor (0x9000)"
    & $Esptool -p $Port write-flash 0x9000 $Nvs
    if ($LASTEXITCODE -ne 0) {
        Fail "NVS geri yuklenemedi! Komutu ELLE tekrar calistir, yoksa WiFi ve cihaz kimligi kayip:`n" +
             "    $Esptool -p $Port write-flash 0x9000 $Nvs"
    }
    Ok "NVS geri yuklendi - WiFi ve cihaz kimligi korundu"
}

# --- Bitti -----------------------------------------------------------------
Write-Host ""
Write-Host "TAMAMLANDI" -ForegroundColor Green
Write-Host ""
Write-Host "Simdi USB kabloyu cikar tak (sadece reset degil)." -ForegroundColor Yellow
Write-Host ""
Write-Host "Test:"
Write-Host "  guc butonu tek tik   -> ekranda 'Ses <deger>'"
Write-Host "  guc butonu cift tik  -> ekranda 'Parlaklik <deger>'"
Write-Host ""
Write-Host "Geri donmek istersen:"
Write-Host "  .\flash.ps1 -Firmware stok-turkce.bin"