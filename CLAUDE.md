# CLAUDE.md — Bu fork hakkında bilmen gerekenler

> Bu dosya, bu depoda çalışan AI ajanı için yazıldı. Genel proje mimarisi için köküdeki
> upstream `AGENTS.md` dosyasını oku — burada **sadece bu fork'a ve bu fiziksel cihaza özgü**
> bilgiler var. İkisi çelişirse bu dosya kazanır.

---

## 1. Bu fork ne için var

`78/xiaozhi-esp32` fork'u. Amaç: **tek bir fiziksel cihaz** için özelleştirilmiş firmware üretmek.
Upstream'e PR göndermek gibi bir hedef yok; değişiklikler kişisel kullanım için.

- Çalışma dalı: **`pwr-button`** — tüm değişiklikler buraya
- `main` dalı upstream ile senkron tutulur, dokunulmaz
- Derleme **GitHub Actions'ta** yapılır (aşağıya bak), yerelde ESP-IDF **kurulu değil** ve kurulması gerekmiyor

---

## 2. Hedef cihaz

**Waveshare ESP32-S3-Touch-LCD-1.83** (Spotpear aynı kartı farklı isimle satıyor)

| | |
|---|---|
| Board dizini | `main/boards/waveshare/esp32-s3-touch-lcd-1.83/` |
| build.py board argümanı | `waveshare/esp32-s3-touch-lcd-1.83` |
| `--name` | `esp32-s3-touch-lcd-1.83` |
| Kconfig | `CONFIG_BOARD_TYPE_WAVESHARE_ESP32_S3_TOUCH_LCD_1_83` |
| Çip | ESP32-S3 (QFN56) rev v0.2, 8MB PSRAM, **16MB flash** |
| Ekran | ST7789P, **240×284**, dokunmatik CST816D/S |
| Ses | ES8311 (çıkış) + ES7210 (giriş), `BoxAudioCodec` |
| Güç | AXP2101 PMIC, 400mAh LiPo |
| Diğer | QMI8658 IMU, PCF85063 RTC, microSD yuvası, USB-C |
| USB | Native **USB-Serial/JTAG** (`303a:1001`), köprü çip yok |

### Üretici kaynakları

`waveshareteam/ESP32-S3-Touch-LCD-1.83` (GitHub) — şema, fabrika firmware'i ve örnekler:
`01_AXP2101` (güç/pil), `02_lvgl_demo_v9`, `03_esp-brookesia` (ürün fotoğraflarındaki renkli
telefon-benzeri arayüz; ayrı firmware, xiaozhi'ye takılamaz), `04_Immersive_block` (IMU),
`05_Spec_Analyzer` (ses spektrumu), `06_videoplayer` (SD'den video).
BSP bileşeni: `waveshare/esp32_s3_touch_lcd_1_83` — SD pinleri buradan doğrulandı.

### Pinler (`config.h`'dan, doğrulanmış)

```
BOOT_BUTTON_GPIO   GPIO_NUM_0    (dişli ikonlu yan buton)
PWR_BUTTON_GPIO    GPIO_NUM_41   (güç ikonlu yan buton)
AUDIO_I2S: MCLK 16, WS 45, BCLK 9, DIN 10, DOUT 8, PA 46
AUDIO_CODEC_I2C:   SDA 15, SCL 14
DISPLAY: CS 5, MOSI 7, CLK 6, DC 4, RST 38, BACKLIGHT 40
TOUCH:   RST 39, INT 13
microSD: D0 3, CMD 1, CLK 2  (SDMMC 1-bit, CD/WP pini yok)
```

### Partition tablosu (`partitions/v2/16m.csv`)

```
nvs       0x9000     16 KB   ← WiFi + board/uuid  ⚠️ KRİTİK
otadata   0xd000      8 KB
phy_init  0xf000      4 KB
ota_0     0x20000    ~4 MB
ota_1     (auto)     ~4 MB
assets    0x800000    8 MB   ← emoji, font, wake word
```

---

## 3. Yapılmış değişiklikler

### PWR butonuna ses ve parlaklık (commit `fa11253`) ✅ cihazda test edildi, çalışıyor

`main/boards/waveshare/esp32-s3-touch-lcd-1.83/esp32-s3-touch-lcd-1.83.cc`

Stok firmware'de `PWR_BUTTON_GPIO` `config.h`'da tanımlıydı ama `.cc` içinde **sıfır referansı** vardı.
Eklenen davranış:

| Hareket | Sonuç |
|---|---|
| Tek tık | Ses: `20 → 40 → 60 → 80 → 100 → 0(sessiz) → 20 …` + ekran bildirimi |
| Çift tık | Parlaklık: `25 → 50 → 75 → 100 → 25 …` + ekran bildirimi, NVS'e kalıcı |
| Uzun basma | **Bilinçli olarak boş** — 4 sn basılı tutmak AXP2101 tarafından donanımsal kapatmaya ayrılmış (`WriteReg(0x27, 0x10)`) |

Değişiklikler: `#include "assets/lang_config.h"`, `Button pwr_button_;` üyesi,
constructor init listesine `pwr_button_(PWR_BUTTON_GPIO)`, `InitializeButtons()` içine üç handler.

**Buton polaritesi aktif-düşük** (`Button` varsayılanı) — cihazda doğrulandı, çalışıyor.
Değiştirme.

### Dokunmatik arayüz — `settings_panel_display.h` ✅ panel cihazda test edildi

Board dizininde yeni, **header-only** dosya: `SettingsPanelDisplay : SpiLcdDisplay`.
Board `.cc`'de sadece iki yer değişti (include + display nesnesinin oluşturulduğu blok).

Arayüz artık **telefon gibi**: açılışta menü (launcher), ikona dokununca o uygulama tam ekran
açılır, başlıktaki geri okuyla menüye dönülür. Sohbet ve saat "kabuksuz" iki görünüm — panel
gizlenir, altta gözler/saat kalır.

| Hareket | Sonuç |
|---|---|
| Ekrana dokunma (sohbet/saat görünümünde) | `ToggleChatState()` — Türkçe wake word imkânsız olduğu için asıl kullanım yolu |
| Aşağı kaydırma | Menü açılır (yedek: üst 28 px'lik görünmez şeride dokunma) |
| Yukarı kaydırma | Menü kapanır |
| Menü ikonu | Sohbet / Saat / Ayarlar / WiFi / Bilgi / **Alarm** / Kısayol |
| Geri oku | Menüye döner |

Menü ızgarası **3 sütun × 66×74 px** (7 uygulama iki sütuna sığmıyordu). 240 px'e ancak
`3*66 + 2*4 = 206` ile giriyor; hücre veya boşluk büyütülürse taşar.

Tasarım kararları — bozmadan önce sebebini oku:

- Panel **`lv_layer_top()`** üzerinde. Upstream'in `SetupUI()`/`SetTheme()` fonksiyonları sadece
  `lv_screen_active()` çocuklarına dokunuyor, böylece çakışmıyoruz. Bedeli: yazı tipi/rengi
  mirasla gelmiyor, `StylePanel()` içinde elle veriliyor.
- **Ses** sadece parmak kalkınca uygulanıyor — `SetOutputVolume` her çağrıda NVS'e yazıyor.
- **Parlaklık** sürüklerken `permanent=false`, bırakınca `true`. Alt sınır 5 (0 = ekran kilidi).
- **`SetTheme` `Application::Schedule` ile** çağrılıyor; LVGL görevinden çağırınca kilit riski var.
- Kaydırma da `LV_EVENT_CLICKED` üretebiliyor → `gesture_handled_` bayrağı, paneli açan
  kaydırmanın aynı anda sohbeti başlatmasını engelliyor.
- Ust bar `pad_left/right` 8 → **20**, `pad_top` 4 → **10**: yuvarlak köşeler wifi/pil ikonlarını
  kırpıyordu. Cihazda doğrulandı, düzeldi.

### Gözler + boşta saat — `eyes_face.h` ✅ cihazda test edildi

Emoji resimleri yerine çizilmiş animasyonlu gözler; upstream'in `emoji_box_`'ı gizleniyor,
`SetEmotion` 21 ifadeyi göz tablosuna çeviriyor. **15 saniye** etkileşim olmazsa saat ekranı:
iki `lv_arc` halka (saniye camgöbeği, pil yeşil/kırmızı), ortada saat + tarih + hava durumu.

⚠️ `LvglDisplay::UpdateStatusBar` her 10 saniyede `SetStatus("HH:MM")` çağırıyor. Bunu etkileşim
sayarsak boşta sayacı 15 saniyeye **hiç** ulaşmıyor ve saat ekranı açılmıyor. 5 karakterli,
3. karakteri `:` olan durum yazıları yok sayılıyor (cihazda ölçüldü, düzeldi).

### IMU (QMI8658) — `imu_qmi8658.h` ✅ cihazda test edildi

Sadece ivmeölçer. Eline alınca/masaya vurunca ekran uyanır; **ekranı masaya kapatınca ses kapanır**,
düzeltince geri açılır.

⚠️ Eksen: cihaz **dik dururken** x=+1.0, z=0.0; **yüzüstü** z=−1.0. Bir tur "masaya düz koy"
varsayımıyla X'e geçtim ve çalışan davranışı bozdum. Doğrusu Z + histerezis
(`kFaceDownZ=-0.60`, `kNotFaceDownZ=-0.30`). Sesi geri açma koşulu "sırtüstü" değil
**"yüzüstü değil"** olmalı — ilk sürümün asıl hatası buydu.

### Hava durumu ✅ / Alarm + RTC (PCF85063) — cihazda test edilmedi

- **Hava durumu**: `mcp-search` servisinin `/weather` ucundan (`https://hava.shoptimize.com.tr/weather`),
  açılıştan 30 sn sonra + 20 dakikada bir. Sunucu tarafı 20 dk önbellekli, her istek Gemini araması
  harcamıyor. Servis Hetzner'e taşınınca tek adres kaldı; eski LAN yedeği anlamını yitirdi.
- **RTC**: `rtc_pcf85063.h`, adres `0x51`. Sistem saati normalde `ota.cc`'deki "Server-Time"
  yanıtından geliyor (**timezone_offset eklenmiş halde**, yani sistem saati yerel duvar saati;
  `TZ` kurulu değil, `localtime` = UTC = yerel). Ağ yokken cihaz 1970'te kalıyordu; artık
  açılışta RTC'den okunuyor ve 5 dakikada bir sistem → RTC geri yazılıyor.
- **Alarm**: tek alarm, her gün aynı saatte. NVS `Settings("alarm")` (`hour`/`minute`/`on`).
  Yüz zamanlayıcısı (1 sn) tetikliyor; çalınca saat ekranına geçip `OGG_VIBRATION`'ı 3 saniyede
  bir en fazla 1 dakika çalıyor, ekrana dokunmak susturuyor. MCP: `self.alarm.set` / `self.alarm.get`.

### Web radyo + sesli bildirim ✅ / segment saat + hareketli gözler ✅

- **Segment saat** (`segment_clock.h`): VolosR/pocketClock görünümü, 240×284'e ölçeklenmiş.
  Rakamlar font değil, **çizilmiş yedi dikdörtgen** (referansın G7 fontu 187 KB ve ticaride
  lisanslı). Ayarlar'daki anahtarla halkalı saatle değiştiriliyor, tercih NVS'te.
- **Gözler eğimi takip ediyor** (`eyes_face.h`): bebek + parlama noktası, yay-sönüm fiziği,
  50 ms'lik ayrı animasyon zamanlayıcısı. Sallayınca sersemleme → kızgın → normal.
  `SetGazeTarget`/`TriggerDizzy` bilerek yalnızca üye yazıyor, IMU görevinden kilitsiz çağrılıyor.
- **Web radyo** (`radio_player.h`): `http->Read → OggDemuxer::Process → PushPacketToDecodeQueue`.
  `wait=true` kuyruk dolunca bloklayıp HTTP okumasını gerçek zamana kilitliyor — akış kontrolü
  bedava. Ses çıkışını oynatma görevi kendi açıyor.
- **Sesli bildirim**: cihaz 30 sn'de bir sunucuya soruyor, bekleyen varsa çalıyor.

⚠️ **`OggDemuxer` yığında oluşturulmaz.** İçinde paket tamponu var (upstream 8192'den 2048'e
indirdi ama hâlâ büyük); yığında oluşturmak görevin yığınını yiyip cihazı çökertiyor. `PlaySound`
da bu yüzden `make_unique` kullanıyor. Radyo bir sürüm bu yüzden açılışta çöktü.

⚠️ **TLS'i LVGL görevinden çalıştırma.** İstasyon listesini arayüz görevinden çekiyordum; çalıştı
ama aynı sınıftan hata. Ağ işleri kısa ömürlü kendi görevlerinde (≥8 KB yığın).

⚠️ `I2cDevice::ReadReg` içinde `ESP_ERROR_CHECK` var — çip yoksa cihaz komple çöker. Hem IMU hem
RTC önce `i2c_master_probe` ile yoklanıyor, okumalar da elle (`i2c_master_transmit_receive`)
yapılıp hata yutuluyor. Yeni I2C çipi eklerken bu kalıbı kopyala.

---

## 4. Derleme — GitHub Actions

> **Upstream senkronu `merge` ile yapıldı (16 Ağu 2026), rebase ile değil.** 61 commit'i tek tek
> oynatmak yerine birleştirildi; fork upstream'e PR göndermiyor, tek çakışma `main/CMakeLists.txt`
> oldu. Bir sonraki senkronda da aynısını yap: `git fetch upstream && git merge upstream/main`.
> Kazanılanlar: **notify** (sunucudan sesli bildirim akışı), yeniden yazılmış `OggDemuxer`
> (2 KB tampon, paket süresi Opus TOC baytından), boşta saat düzeltmesi.

Yerelde ESP-IDF kurulu değil. Derleme `.github/workflows/agon-build.yml` ile yapılır.

> **Çökme/donma ayıklama:** artifact'a `build/xiaozhi.elf` de konuyor ve workflow'un
> `backtrace` girdisi var. Seri porttan alınan adresleri çözmek için:
> `gh -R ccorayy/xiaozhi-esp32 workflow run agon-build.yml --ref pwr-button -f backtrace="0x4201... 0x4202..."`
> sonra `gh run view <id> --log`. Yerelde xtensa toolchain yok, çözüm CI'da yapılıyor.
> ⚠️ Adreslerin anlamlı olması için ELF, cihazdaki firmware ile **aynı kaynaktan**
> derlenmiş olmalı; sadece workflow dosyası değiştiyse adresler kayar değil.

```yaml
name: Agon Firmware
on: [workflow_dispatch, push → branches: pwr-button]
container: espressif/idf:v6.0.2
run: python scripts/build.py waveshare/esp32-s3-touch-lcd-1.83 \
       --name esp32-s3-touch-lcd-1.83 --language tr-TR
artifact: xiaozhi-1.83-turkce-pwrbutton → build/merged-binary.bin
```

**`pwr-button` dalına her push otomatik derleme tetikler.** Süre ~5-6 dakika.

Artifact'ı **ajan indirir ve cihaza yükler** (bkz. §9 kural 4): `gh run download <id>`.
Cihazda **test etmek** kullanıcıda — ekranı göremezsin, "çalışıyor" deme.

### `scripts/build.py` faydalı seçenekler

```bash
--list-boards --json     # board/name/build_options şeması
--list-languages         # tr-TR listede var
--list-wake-words        # hepsi İngilizce/Çince, Türkçe YOK
--build-options-json     # display_style, multiline_chat, aec_mode, wifi_provisioning
```

---

## 5. ⚠️ Flash prosedürü — NVS adımı zorunlu

**Sahada doğrulandı (11 Ağu 2026):** üretilen `merged-binary.bin` **9.651.268 byte** ve esptool
`0x0`–`0x934FFF` aralığını siliyor. NVS (`0x9000`–`0xD000`) bu aralığın **içinde**.

esptool dokümantasyonu: *"gaps between the input files are padded with 0xFF bytes … any flash
sectors between the individual files will be erased."*

Sonuç: sadece firmware yazılırsa cihaz **yeni bir `board/uuid` üretir** ve xiaozhi.me'de
**ikinci bir cihaz** olarak kaydolur. Bu bir kez yaşandı ve NVS geri yazılarak düzeltildi.

**Doğru sıra (depo kökündeki `flash.ps1` bunu yapar):**

```powershell
.\esptool.exe -p COM8 write-flash 0x0    merged-binary.bin
.\esptool.exe -p COM8 write-flash 0x9000 nvs-only.bin
```

`nvs-only.bin` = cihazın **güncel** NVS'i (16384 byte).

> ⚠️ **Flash'tan önce NVS'i cihazdan taze oku.** Eski bir yedeği geri yazmak, kullanıcının
> o yedekten sonra yaptığı ayarları siler. Bu bir kez yaşandı: `ota_url` (kendi sunucusu)
> her flash'ta silinip cihaz Çin'e dönüyordu, üstelik sessizce — sadece "Çin sürümü açıldı"
> olarak fark edildi. Doğru sıra:
>
> ```powershell
> .\esptool.exe -p COM8 read-flash 0x9000 0x4000 nvs-only.bin   # ÖNCE oku
> .\esptool.exe -p COM8 write-flash 0x0    merged-binary.bin
> .\esptool.exe -p COM8 write-flash 0x9000 nvs-only.bin          # sonra geri yaz
> ```
>
> Okunan dosyada `ota_url`, `keenetic`, SSID ve `board/uuid` dizgilerinin bulunduğunu
> doğrula (ASCII araması yeter). `nvs-eski-2026-08-11.bin` ilk yedek, dokunma.

> **NVS'te `Get-FileHash` karşılaştırması yapma.** NVS log yapılı bir depodur, cihaz her
> açılışta içine yazar. Doğru yedek geri yazılsa bile hash tutmaz. Doğrulama için
> anahtarlara bak (`board/uuid`, `wifi/ssid`).

---

## 6. Doğrulanmış API referansı

Kod yazarken bunları varsayma, aşağıdakiler kaynaktan teyit edildi:

| Çağrı | Dosya |
|---|---|
| `Button::OnClick / OnDoubleClick / OnPressDown / OnLongPress / OnMultipleClick` | `main/boards/common/button.h` |
| `Board::GetAudioCodec() / GetBacklight() / GetDisplay()` | `main/boards/common/board.h:70,72,74` |
| `AudioCodec::SetOutputVolume(int)`, `output_volume()` — varsayılan 70 | `main/audio/audio_codec.h:32,47` |
| `Backlight::SetBrightness(uint8_t, bool permanent)`, `brightness()` | `main/boards/common/backlight.h` |
| `Display::ShowNotification(const std::string&, int ms)` | `main/display/display.h:39` |
| `Display::SetupUI() / SetTheme() / SetEmotion() / SetChatMessage()` | `main/display/display.h`, `main/display/lcd_display.h` |
| `LcdDisplay` / `SpiLcdDisplay` — `SetupUI()` override edilebilir | `main/display/lcd_display.h:14,51,60` |
| `McpServer::AddTool(name, desc, PropertyList, cb)` | `main/mcp_server.h` |
| `Lang::Strings::VOLUME / MUTED / MAX_VOLUME` | `main/assets/locales/tr-TR/language.json` |
| `Application::Schedule(std::function<void()>&&)` — ana görevde çalıştırır | `main/application.h:80` |
| `Application::ToggleChatState()` — sadece event biti set eder, her görevden güvenli | `main/application.cc:700` |
| `WifiBoard::EnterWifiConfigMode()` — NVS'teki WiFi'ı **silmez**, portalı açar | `main/boards/common/wifi_board.cc:195` |
| `WifiManager::IsConnected/GetSsid/GetIpAddress/GetRssi` — **tarama API'si yok** | `<wifi_manager.h>` (78/esp-wifi-connect) |
| `SsidManager::AddSsid/RemoveSsid/SetDefaultSsid/GetSsidList` — çoklu ağ, NVS'i kendi yazar | `<ssid_manager.h>` (aynı bileşen) |

Türkçe string değerleri: `VOLUME`="Ses ", `MUTED`="Sessiz", `MAX_VOLUME`="Maksimum ses".
**Parlaklık için hazır string yok**, literal kullanılıyor.

`<string>` zaten `board.h` üzerinden geliyor.

---

## 7. Bilinen tuzaklar

| Konu | Gerçek |
|---|---|
| **Dokunmatik** | Upstream'de hiçbir tıklanabilir widget yok. Bu fork'ta `settings_panel_display.h` ile kullanılıyor (bkz. §3). Kaydırma olayı parmağın altındaki nesneye gider; `container_`/`emoji_box_` üzerinde `EVENT_BUBBLE` ile ekrana çıkarılıyor ve scroll'un hareketi yutmaması için o ikisinde `SCROLLABLE` kapatılıyor. |
| **Kurucuda ağ kullanmak** | Board kurucusu (`InitializeXxx`) çalışırken **lwIP ayakta değil**. Orada soket açan bir şey çağırmak (`httpd_start`, `socket()`, DNS) `assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)` verip **açılış döngüsü** yaratıyor. Bir kez yaşandı: SD dosya sunucusu tercihi NVS'ten okunup kurucuda başlatılıyordu; kullanıcı sunucuyu açık bırakınca cihaz bir daha açılmadı. Doğrusu: zamanlayıcıyla `WifiManager::IsConnected()` bekle, sonra `Application::Schedule` ile başlat. Aynı sebeple hava durumu da açılıştan 30 sn sonra çekiliyor. |
| **LVGL'i kilitsiz cagirmak** | Cihaz **donuyor** (yeniden baslatmiyor, ekran son kareyi tutuyor), seri portta `task_wdt: IDLE0` + `CPU 0: esp_timer` ve `lv_inv_area` icinde sonsuz dongu. Sebep: `Display` sanal metotlari (`SetStatus`, `SetChatMessage`, `SetEmotion`) LVGL gorevinden **degil** ana gorev ve `PowerSaveTimer`'in esp_timer gorevinden de cagriliyor (`OnEnterSleepMode` → `SetPowerSaveMode` → `SetChatMessage`). Override edip icinde LVGL'e dokunuyorsan **`DisplayLockGuard` sart**. Kilit ozyinelemeli (`xSemaphoreTakeRecursive`), ust sinif da kilitliyor olsa bile ic ice almak guvenli. |
| **`-Werror` enum** | `LV_PART_x \| LV_STATE_x` doğrudan OR'lanınca `-Werror=deprecated-enum-enum-conversion` derlemeyi durduruyor. `lv_style_selector_t`'ye cast et. Bir CI turu bu yüzden yandı. |
| **Kapalı LVGL widget'ları** | `sdkconfig.defaults`'ta flash tasarrufu için `=n`: **tileview, tabview, keyboard, list, menu, msgbox, spinner, chart, calendar, span, spinbox, led, win, animimg**. Kullanmaya kalkarsan "was not declared in this scope" alırsın — bir CI turu tileview yüzünden yandı. Sayfalama `lv_obj` + `lv_obj_set_scroll_snap_x` + `SCROLL_ONE` ile kendimiz yapıldı. `slider`, `switch`, `button`, `buttonmatrix` **açık**. Paylaşılan sdkconfig'i değiştirmek tüm board'ları etkiler, son çare olsun. |
| **microSD** | ✅ **Bu fork'ta çalışıyor** — 64 GB FAT32 kart cihazda doğrulandı (`59.5 GB OK`). Pinler stok `config.h`'da **yoktu**, Waveshare BSP bileşeninden alındı. `main/CMakeLists.txt`'te SDMMC bağımlılığı bu board için de eklendi. **Ama hâlâ tüketicisi yok** — sadece `/sdcard` mount ediliyor. `format_if_mount_failed=false`, asla formatlama. Upstream issue #1053 hâlâ açık. |
| **Wake word Türkçe** | **Mümkün değil.** ESP-SR WakeNet/MultiNet sadece İngilizce + Mandarin. `--list-wake-words` çıktısında Türkçe yok. Çözüm: İngilizce wake word veya dokunmatik/buton ile push-to-talk. |
| **Arayüz dili** | `LANGUAGE_TR_TR` var (38 dilden biri), `--language tr-TR` çalışıyor. Ama **diyalog** dili sunucu tarafında belirleniyor. |
| **Sunucu** | `wifi/ota_url` NVS'te **boş** → `CONFIG_OTA_URL` = `https://api.tenclass.net/xiaozhi/ota/` (Çin). Değiştirmek için: WiFi portal `192.168.4.1` → Advanced → `ota_url`. `wifi_board.cc:59` `show_ota_config = true` (doğrulandı). Derleme gerekmez. |
| **esptool büyük okuma** | 16MB tek seferde `read-flash` "Packet content transfer stopped" ile patlar (esptool issue #745). 2MB'lık parçalar halinde oku veya `--no-stub`. |
| **`erase-flash`** | Asla önerme. NVS'i siler. |

---

## 8. Yol haritası (kullanıcının ilgilendiği sıra)

1. ~~**Tam LVGL ayar paneli**~~ ✅ **bitti** — bkz. §3.
2. ~~**Panele WiFi sayfası**~~ ✅ **bitti** — tarama `esp_wifi_scan_start()` + `WIFI_EVENT_SCAN_DONE`
   ile elle yapılıyor (bileşende public tarama API'si yok, kendi taramasıyla çakışma hatası
   yutuluyor). Şifre klavyesi `lv_buttonmatrix` ile (⚠️ `LV_USE_KEYBOARD=n`, `lv_keyboard` yok).
   Kayıtlı ağlar `SsidManager` üzerinden listeleniyor/siliniyor.
3. **Özel emoji/yüz seti** — artık gerekmiyor gibi: emoji yerine çizilmiş gözler kullanılıyor
   (bkz. §3). İstenirse `78/xiaozhi-assets-generator` (tarayıcıda çalışır), 21 ifade, 240×284.
4. **Kullanılmayan çipler** — ✅ ikisi de kullanımda: **QMI8658 IMU** (`imu_qmi8658.h`) ve
   **PCF85063 RTC** (`rtc_pcf85063.h`). Referans kod: `waveshareteam/ESP32-S3-Touch-LCD-1.83`
   içinde `examples/esp-idf/04_Immersive_block` (IMU) ve `libraries/SensorLib` (RTC).
5. **microSD** — ✅ mount çalışıyor, **sıradaki iş**. Kullanıcının kart okuyucusu yok; PC'den
   karta dosya atmak için **cihaz üzerinde WiFi yükleme sayfası** yapılacak (USB MSC değil:
   USB-Serial/JTAG ile OTG aynı PHY'yi paylaşıyor, MSC'ye geçmek kabloyla flash'ı feda eder).
   Sonrasında ne için kullanılacağı:
   (Waveshare örnek deposundaki `06_videoplayer` SD'den video oynatıyor — referans kod.)
   - *Görsel/animasyon SD'den:* LVGL'in dosya sistemi sürücüleri (`LV_USE_FS_POSIX` vb.) sdkconfig'de
     **kapalı**, ama `lv_fs_drv_register()` çekirdek API — sürücüyü kendi header'ımızda kayıt
     edebiliriz, sdkconfig'e dokunmadan. `LV_USE_LODEPNG=y` olduğu için PNG çözülüyor.
     Bu, flash'taki 8 MB asset sınırını atlatmanın en temiz yolu.
   - *Yerel ses:* `AudioService::PlaySound` sadece bellekteki Ogg/Opus alıyor, akış yolu yok.
     Kısa bildirim sesleri olur, şarkı için streaming decoder yazmak gerekir.
   - *MCP dosya aracı:* asistanın SD'ye not yazıp okuması — kalıcı hafıza.
6. ~~**Kendi sunucusu**~~ ✅ **kuruldu ve çalışıyor** (12 Ağu 2026) — bkz. §10.

### Eski notlar (kurulum öncesi araştırma, referans için)

   `xinnan-tech/xiaozhi-esp32-server`. Hedef donanım: **Raspberry Pi 5 8 GB**
   (Hetzner CPX22 kullanıcının üretim sunucusu, oraya kurulmadı). Kaynaktan doğrulandı:
   - LLM: **Gemini yerleşik** (`llm/gemini/gemini.py`). **Claude için sağlayıcı yok** — genel
     `llm/openai/openai.py` `base_url` aldığı için OpenAI-uyumlu uç üzerinden denenebilir.
   - ASR: "GroqASR" diye sağlayıcı yok. `asr/openai.py` `base_url`+`api_key`+`model_name` alıyor →
     Groq'un OpenAI-uyumlu ucuna `whisper-large-v3-turbo` ile yönlendirilir. Türkçe çözümü bu.
   - TTS: `tts/edge.py` var, EdgeTTS ücretsiz, `tr-TR-EmelNeural/AhmetNeural`.
   - MCP sunucunun içinde (`server_mcp`, `mcp_endpoint`) → canlı veri araçları oraya bağlanır.
   - Minimal kurulum: tek konteyner, port 8000 (ws) + 8003 (http/OTA), API tabanlı sağlayıcılarla
     ~2 GB. Yerel model mount'unu (`SenseVoiceSmall`, ~1 GB) atla. Tam modül web konsol getirir
     ama MySQL+Redis+Java ister.
   - Cihaz tarafında değişen tek şey `ota_url`; **firmware'e dokunulmuyor**.

⚠️ Konsoldaki model listesinde (Xiaozhi Lite, Qwen 3.6, DeepSeek V4, Doubao, GPT-5) **Claude/Gemini
yok** ve hiçbirinin internet erişimi yok — "maç ne zaman" tipi sorular bu yüzden cevapsız kalıyor.
Canlı veri = MCP aracı meselesi, model meselesi değil.

---

## 9. Ajan için kurallar

1. **Kaynağı oku, varsayma.** Bu depo hızlı değişiyor. Bir API veya sabit hakkında emin değilsen `main/` altında grep'le. Yanlış imza = 6 dakikalık başarısız CI turu.
2. **Sadece `pwr-button` dalında çalış.** `main`'i upstream senkronu için temiz bırak.
3. **Board dosyasına dokunurken minimal ol.** Upstream `git rebase` ile güncellenecek; ne kadar az satır değişirse çakışma o kadar az.
4. **Flash'ı ajan yapıyor, testi kullanıcı.** Kullanıcı bu işi devretti (11 Ağu 2026).
   `C:\xiaozhi` klasöründe `esptool.exe`, `nvs-only.bin`, `stok-turkce.bin` ve geri dönüş için
   `onceki.bin` var; cihaz **COM8**'de. Akış: `gh run download <id>` → `merged-binary.bin`'i
   `onceki.bin` olarak yedekle → `write-flash 0x0` → `write-flash 0x9000 nvs-only.bin`.
   `flash.ps1` aynı işi yapar ama `Read-Host` ile onay sorduğu için ajan oturumunda takılır,
   iki esptool komutunu ayrı çalıştır. **Cihazda test etmek yine kullanıcıda** — ekranı göremezsin,
   "çalışıyor" deme, "şunu dener misin" de.
5. **Sırlar depoya girmez.** Bu fork **public**. Cihazın MAC'i, `board/uuid`'si, WiFi SSID/şifresi, MQTT credential'ları hiçbir commit'e girmemeli. Yerel notlar için `cihaz-notlari.local.md` kullan — `.gitignore`'da.
6. **Değişiklik → push → kullanıcıya haber.** Push otomatik derleme tetikler; kullanıcıya artifact'ın hazır olacağını ve flash sırasını (NVS dahil) hatırlat.
7. **Riskli bir şey önermeden önce geri dönüş yolunu söyle.** Kullanıcının `kritik-yedek.bin` (64KB) ve `nvs-only.bin` (16KB) yedekleri var; tam 16MB yedeği yok.

---

## 10. Kendi sunucusu — Hetzner'de kurulu ve çalışıyor (15 Ağu 2026)

**Cihaz artık xiaozhi.me'ye bağlanmıyor.** Konsoldaki ajan ayarları, hafıza kayıtları ve model
seçimi devre dışı; her şey sunucudaki config dosyasından geliyor.

> **Raspberry Pi 5 kurulumu emekli edildi (15 Ağu 2026).** Ev ağına ve KeenDNS'e bağımlıydı;
> Pi kapanınca asistan tamamen susuyordu. Her şey kullanıcının zaten sahip olduğu Hetzner
> kutusuna taşındı. Pi'deki `~/xiaozhi-server` + `~/xiaozhi-mcp-search` ağacı **olduğu gibi**
> kopyalandı (yalnızca adresler ve port yayınlama değişti), o yüzden aşağıdaki tuzak tablosu
> hâlâ geçerli.

| | |
|---|---|
| Donanım | Hetzner CPX22 `shoptimize-prod`, 2 vCPU / 4 GB / 80 GB, Ubuntu 24.04, x86_64, Nuremberg |
| Adres | `178.104.94.230` — **üretim sunucusu**, üstünde Coolify v4 + shoptimize/WB OTP uygulamaları var |
| SSH | `ssh root@178.104.94.230` — anahtar bu makinede (`~/.ssh/id_ed25519`) |
| Dizin | `/opt/xiaozhi/` — `xiaozhi-server/` ve `xiaozhi-mcp-search/`. Coolify'ın kendi dizinlerine dokunulmadı |
| Bellek | xiaozhi 148 MB + mcp-search 72 MB ≈ **220 MB**. (Eski nottaki "~2 GB" imaj boyutuydu, RAM değil.) Kutuda ~1 GB boşta kalıyor |
| Yayın | Host portu **açılmıyor**. Coolify'ın Traefik'i (`coolify` ağı) TLS'i sonlandırıyor, sertifika Let's Encrypt |
| Cihaz ayarı | `ota_url` = `https://ota.shoptimize.com.tr/xiaozhi/ota/` (WiFi portal → Advanced) |

Alan adları — hepsi Hostinger'da `shoptimize.com.tr` bölgesinde A kaydı, `178.104.94.230`:

| Alan adı | Konteyner portu | Ne |
|---|---|---|
| `ses.shoptimize.com.tr` | 8080 | websocket (`wss://.../xiaozhi/v1/`) |
| `ota.shoptimize.com.tr` | 8090 | cihaz kaydı / firmware güncelleme |
| `hava.shoptimize.com.tr` | 8100 | arama + hava durumu (`/weather`) |

⚠️ Coolify kurulumunda dikkat edilenler:
- `docker-compose.yml`'de **`ports:` yok**. Traefik 80/443/8080'i zaten tutuyor; host portu açmak hem
  çakışırdı hem de açık internete şifresiz uç koyardı.
- Yönlendirme ara katmanı **kendimizin** (`xiaozhi-https`). Coolify'ın `redirect-to-https`'i başka bir
  uygulamanın etiketinde tanımlı; o uygulama silinirse bizimki de kırılırdı.
- Gemini anahtarı compose içinde değil, `secrets.env` dosyasında (chmod 600).
- **Sertifika, DNS kaydından önce istenirse Traefik NXDOMAIN alıp uzun süre yeniden denemiyor.**
  DNS yayıldıktan sonra `docker compose up -d --force-recreate` ile router'ları yeniden kaydettir;
  sertifika 20 saniyede geliyor. Coolify'ın proxy'sini yeniden başlatmaya gerek yok (üretim kesilir).

Sağlayıcılar: **ASR** GroqASR (`whisper-large-v3-turbo`) — Türkçeyi kusursuz tanıyor.
**LLM** Groq `openai/gpt-oss-120b`. **TTS** EdgeTTS `tr-TR-EmelNeural`. Hafıza kapalı.

### Bu kurulumda yanılıp düzelttiğimiz şeyler

Hepsi cihazda "şu an bir sorun var" olarak görünüyordu; log'a bakmadan ayırt edilemez.

| Belirti | Gerçek sebep | Çözüm |
|---|---|---|
| `Unknown field for Schema: minimum` | Yerleşik `gemini` sağlayıcısı eski Google SDK'sını kullanıyor, cihazın araç şemalarındaki `minimum/maximum` alanlarında patlıyor | Aynı modeli **OpenAI uyumlu uçtan** çağır (`type: openai` + `base_url`) |
| Model Çince cevaplıyor, sonra TTS "No audio was received" | Şablondaki `{{language}}` **TTS bloğundaki `language` alanından** okunuyor; boşsa varsayılan Çince. Türkçe ses Çince metni seslendiremiyor | `TTS.EdgeTTS.language: "Turkce"` |
| Sesli cevaba İngilizce düşünme metni karışıyor | `gpt-oss` akıl yürütmeyi `<think>` etiketi **olmadan** content'e akıtıyor; mevcut filtre yakalamıyor, araç yolunda hiç çalışmıyor | `openai.py` içindeki `THINKING_DISABLED_DOMAINS`'e `"groq.com": {"reasoning_format": "hidden"}` — dosya `patches/llm_openai.py` olarak mount edildi |
| Çince metinle TTS kilitleniyor | Yerleşik eklentiler (`play_music`, `get_weather`, haber) **sabit Çince** `ActionResponse` döndürüp doğrudan TTS'e veriyor | `Intent.function_call.functions: []` — hepsi kapatıldı |
| Cümle üç parçaya bölünüyor | `min_silence_duration_ms: 200` çok kısa | 800 |
| Cevaplar 16-24 sn gecikiyor, **log'da hata yok** | Groq ücretsiz katmanı **8000 token/dakika**. 6528 karakterlik şablon × tur başına 3 çağrı = sınır aşımı → 429 → OpenAI SDK sessizce bekleyip yeniden deniyor | Şablonu Türkçe ve ~800 karaktere indir (`data/tr-prompt.txt`, `prompt_template` ile) → **1-2 sn** |
| Gemini'de `429 RESOURCE_EXHAUSTED` | Ücretsiz katman `gemini-3.5-flash` için **günde 20 istek** | Groq'a geçildi. Gemini config'te yedek olarak duruyor |

Model seçerken denenenler: `llama-3.3-70b` Türkçesi bozuk; `qwen3.6` cevaba `<think>` karıştırıyor;
`groq/compound` (gömülü web aramalı) **araç çağırmayı desteklemiyor**, cihaz kontrolü giderdi.

### Canlı veri — MCP arama servisi ✅ (12 Ağu 2026)

İkinci bir konteyner: `/opt/xiaozhi/xiaozhi-mcp-search/` (compose'a `mcp-search` servisi olarak eklendi).
`web_search` adlı tek bir MCP aracı sunuyor; içeride **Gemini'nin yerleşik Google araması**
(`tools:[{google_search:{}}]`) çağrılıyor ve sonuç sesli okunmaya uygun biçimde dönüyor.
xiaozhi'ye `data/.mcp_server_settings.json` ile tanıtıldı (`http://mcp-search:8100/mcp`,
`streamable-http`; iki konteyner aynı compose ağında).

Cihazda doğrulandı: *"Beşiktaş maçı ne zaman?"* → doğru tarih ve saat. Arama turu **~9 sn**
(normal sohbet 1-2 sn); o sırada cihaz sessiz.

Neden ayrı servis — bunlar test edildi:

- Gemini'nin `google_search`'ü **sadece kendi API'sinden** çalışıyor. Sunucunun kullandığı
  OpenAI-uyumlu uç reddediyor: `Unknown name "tools" at 'extra_body.google'`.
- Yerleşik araç + fonksiyon çağırma **birlikte** kullanılabiliyor ama
  `tool_config.include_server_side_tool_invocations: true` şart.
- Grounding **ücretsiz katmanda yok**, ödemeli katman gerekiyor: ayda 5000 arama ücretsiz,
  sonrası $14/1000. Sohbet Groq'ta (ücretsiz) kaldığı için para sadece arama yapılınca harcanıyor.
- `mcp` paketi 2.0'da `mcp.server.fastmcp.FastMCP` → `mcp.server.MCPServer` oldu; API aynı.
- xiaozhi konteynerinde **curl yok**, hata ayıklarken `python` + `httpx` kullan.

### Bilinen eksikler
- **Yama kırılgan.** `patches/llm_openai.py` imajın içindeki dosyanın üzerine biniyor; imaj
  güncellenirse yeniden üretilmeli (komut `docker-compose.yml` içinde yorumda).
- Sunucu üretim kutusunu paylaşıyor; RAM'in ~1 GB'ı boşta, ağır bir şey eklerken ölç.
- Sunucudaki API anahtarları `data/.config.yaml` içinde düz metin (chmod 600). **Depoya girmemeli.**

Geri dönüş: WiFi portal → Advanced → `ota_url` alanını boşalt; cihaz `api.tenclass.net`'e döner.
