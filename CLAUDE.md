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

### Pinler (`config.h`'dan, doğrulanmış)

```
BOOT_BUTTON_GPIO   GPIO_NUM_0    (dişli ikonlu yan buton)
PWR_BUTTON_GPIO    GPIO_NUM_41   (güç ikonlu yan buton)
AUDIO_I2S: MCLK 16, WS 45, BCLK 9, DIN 10, DOUT 8, PA 46
AUDIO_CODEC_I2C:   SDA 15, SCL 14
DISPLAY: CS 5, MOSI 7, CLK 6, DC 4, RST 38, BACKLIGHT 40
TOUCH:   RST 39, INT 13
microSD (xiaozhi KULLANMIYOR, pinler boş): D0 3, CMD 1, CLK 2
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

| Hareket | Sonuç |
|---|---|
| Ekrana dokunma | `ToggleChatState()` — Türkçe wake word imkânsız olduğu için asıl kullanım yolu |
| Aşağı kaydırma | Panel açılır (yedek: üst 28 px'lik görünmez şeride dokunma) |
| Yukarı kaydırma | Panel kapanır |
| Sağa/sola kaydırma | Sayfalar: Ayarlar / Bilgi / Kısayollar |

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

---

## 4. Derleme — GitHub Actions

Yerelde ESP-IDF kurulu değil. Derleme `.github/workflows/agon-build.yml` ile yapılır.

```yaml
name: Agon Firmware
on: [workflow_dispatch, push → branches: pwr-button]
container: espressif/idf:v6.0.2
run: python scripts/build.py waveshare/esp32-s3-touch-lcd-1.83 \
       --name esp32-s3-touch-lcd-1.83 --language tr-TR
artifact: xiaozhi-1.83-turkce-pwrbutton → build/merged-binary.bin
```

**`pwr-button` dalına her push otomatik derleme tetikler.** Süre ~5-6 dakika.

Kullanıcı artifact'ı Actions sekmesinden indirir ve cihaza kendisi yükler.
Ajan cihaza erişemez — **flash adımını asla "yapıldı" diye varsayma, kullanıcıya sor.**

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

`nvs-only.bin` = kullanıcının 64KB kritik yedeğinin `0x9000..0xCFFF` aralığı (16384 byte).

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
| **`-Werror` enum** | `LV_PART_x \| LV_STATE_x` doğrudan OR'lanınca `-Werror=deprecated-enum-enum-conversion` derlemeyi durduruyor. `lv_style_selector_t`'ye cast et. Bir CI turu bu yüzden yandı. |
| **Kapalı LVGL widget'ları** | `sdkconfig.defaults`'ta flash tasarrufu için `=n`: **tileview, tabview, keyboard, list, menu, msgbox, spinner, chart, calendar, span, spinbox, led, win, animimg**. Kullanmaya kalkarsan "was not declared in this scope" alırsın — bir CI turu tileview yüzünden yandı. Sayfalama `lv_obj` + `lv_obj_set_scroll_snap_x` + `SCROLL_ONE` ile kendimiz yapıldı. `slider`, `switch`, `button`, `buttonmatrix` **açık**. Paylaşılan sdkconfig'i değiştirmek tüm board'ları etkiler, son çare olsun. |
| **microSD** | Firmware **hiç kullanmıyor**. Board dosyasında 0 referans; `main/CMakeLists.txt` SDMMC sürücülerini sadece ESP32-P4 EV board için linkliyor. Assets flash partition'ında, müzik forkları HTTP stream ediyor. Upstream issue #1053 açık. |
| **Wake word Türkçe** | **Mümkün değil.** ESP-SR WakeNet/MultiNet sadece İngilizce + Mandarin. `--list-wake-words` çıktısında Türkçe yok. Çözüm: İngilizce wake word veya dokunmatik/buton ile push-to-talk. |
| **Arayüz dili** | `LANGUAGE_TR_TR` var (38 dilden biri), `--language tr-TR` çalışıyor. Ama **diyalog** dili sunucu tarafında belirleniyor. |
| **Sunucu** | `wifi/ota_url` NVS'te **boş** → `CONFIG_OTA_URL` = `https://api.tenclass.net/xiaozhi/ota/` (Çin). Değiştirmek için: WiFi portal `192.168.4.1` → Advanced → `ota_url`. `wifi_board.cc:59` `show_ota_config = true` (doğrulandı). Derleme gerekmez. |
| **esptool büyük okuma** | 16MB tek seferde `read-flash` "Packet content transfer stopped" ile patlar (esptool issue #745). 2MB'lık parçalar halinde oku veya `--no-stub`. |
| **`erase-flash`** | Asla önerme. NVS'i siler. |

---

## 8. Yol haritası (kullanıcının ilgilendiği sıra)

1. ~~**Tam LVGL ayar paneli**~~ ✅ **bitti** — bkz. §3.
2. **Panele WiFi sayfası** — sıradaki iş. İki parça: (a) kayıtlı ağ listesi, varsayılan seç/sil —
   `SsidManager` ile kolay, klavye gerekmiyor; (b) yeni ağ eklemek için şifre girişi — 240 px'de
   tuş başına 24 px, eziyetli ama yapılabilir. ⚠️ `LV_USE_KEYBOARD=n`, `lv_keyboard` yok;
   `lv_buttonmatrix` açık, klavyeyi onunla kurmak gerekir.
   ⚠️ Ağ **taraması** için bileşende public API yok, `esp_wifi_scan_start()` doğrudan
   çağrılacak; bileşen de arka planda kendi taramasını yapıyor, çakışma hatasını yakala.
3. **Özel emoji/yüz seti** — `78/xiaozhi-assets-generator` (tarayıcıda çalışır), 21 ifade, 240×284. OTA ile iner, flash gerekmez, kod değişikliği yok.
4. **Kullanılmayan çipler** — kartta **QMI8658 IMU** ve **PCF85063 RTC** var, `main/` ağacında
   sürücüleri **yok** (tek referans başka bir board'un `pin_config.h`'si). Sıfırdan I2C sürücüsü
   yazmak gerekir. IMU > RTC: eline alınca uyandırma, ters çevirince sessize alma.
5. **microSD** — yuva var, firmware'de sıfır kod. Mount etmek kolay (örnek:
   `nologo/xingzhi-abs-2.0`) ama **tüketici yok**: `AudioService::PlaySound` sadece bellekteki
   Ogg/Opus alıyor, akış yolu yok. Asset'leri SD'den okumak 8 MB sınırını kaldırır ama yükleyiciyi
   değiştirmek gerekir. En kısa faydalı yol: mount + MCP dosya aracı.
6. **Kendi sunucusu** — `xinnan-tech/xiaozhi-esp32-server`. Hedef donanım: **Raspberry Pi 5 8 GB**
   (Hetzner CPX22 kullanıcının üretim sunucusu, oraya kurulmayacak). Kaynaktan doğrulandı:
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
4. **Cihaza erişimin yok.** Flash ve test kullanıcıda. "Test ettim" deme, "flash'layıp şunu dener misin" de.
5. **Sırlar depoya girmez.** Bu fork **public**. Cihazın MAC'i, `board/uuid`'si, WiFi SSID/şifresi, MQTT credential'ları hiçbir commit'e girmemeli. Yerel notlar için `cihaz-notlari.local.md` kullan — `.gitignore`'da.
6. **Değişiklik → push → kullanıcıya haber.** Push otomatik derleme tetikler; kullanıcıya artifact'ın hazır olacağını ve flash sırasını (NVS dahil) hatırlat.
7. **Riskli bir şey önermeden önce geri dönüş yolunu söyle.** Kullanıcının `kritik-yedek.bin` (64KB) ve `nvs-only.bin` (16KB) yedekleri var; tam 16MB yedeği yok.
