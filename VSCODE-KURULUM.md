# VS Code'a Taşıma — Tek Seferlik Kurulum

Bundan sonra iş akışın şöyle olacak:

```
VS Code'da Claude'a ne istediğini söyle
   → o dosyaları düzenler, commit'ler, push'lar
   → GitHub Actions ~6 dakikada derler
   → sen artifact'ı indirip .\flash.ps1 çalıştırırsın
   → cihazda test edip geri bildirim verirsin
```

Konuşma geçmişi ve tüm proje bağlamı **`CLAUDE.md`** dosyasında kalıcı olarak duruyor —
Claude her yeni oturumda onu otomatik okuyor.

---

## 1. Gerekenler

| Araç | Nereden | Not |
|---|---|---|
| **Git** | https://git-scm.com/download/win | Kurulumda varsayılanları kabul et |
| **VS Code** | https://code.visualstudio.com | |
| **Claude Code eklentisi** | VS Code → Extensions (`Ctrl+Shift+X`) → "Claude Code" ara → Install | |

Kurulumdan sonra VS Code'u kapat aç.

---

## 2. Depoyu bilgisayarına klonla

VS Code'da **Terminal → New Terminal** (`` Ctrl+` ``), sonra:

```powershell
cd C:\
git clone -b pwr-button https://github.com/ccorayy/xiaozhi-esp32.git xiaozhi-proje
cd C:\xiaozhi-proje
code -r .
```

VS Code artık projeyi açtı ve `pwr-button` dalındasın.

İlk kez push yaparken GitHub girişi isteyecek — tarayıcı açılır, onaylarsın.

Git kimliğini bir kez ayarla:

```powershell
git config --global user.name "ccorayy"
git config --global user.email "GITHUB-EPOSTAN"
```

---

## 3. Bu üç dosyayı projeye ekle

Sana gönderdiğim dosyaları `C:\xiaozhi-proje` klasörüne kopyala:

| Dosya | Nereye | Ne işe yarar |
|---|---|---|
| `CLAUDE.md` | kök dizin | **En önemlisi.** Claude'un proje hafızası — cihaz bilgileri, doğrulanmış API'ler, tuzaklar, yol haritası |
| `flash.ps1` | kök dizin | Güvenli flash betiği — NVS geri yüklemeyi unutturmuyor |
| `.gitignore` eki | aşağıya bak | Yedeklerin ve sırların depoya girmesini engeller |

`.gitignore` dosyasını aç, **en altına** şunları ekle:

```gitignore
# --- kisisel / gizli ---
*.bin
!.github/**/*.bin
cihaz-notlari.local.md
esptool.exe
```

Sonra commit'le:

```powershell
git add CLAUDE.md flash.ps1 .gitignore
git commit -m "Proje baglami, flash betigi ve gitignore"
git push
```

> ⚠️ Bu push da bir derleme tetikler (kod değişmediği için sonuç aynı olur, zararsız).

---

## 4. Cihaz notlarını yerelde tut

Fork'un **public**. Cihazın MAC'i, UUID'si, WiFi şifresi hiçbir commit'e girmemeli.

Kök dizinde **`cihaz-notlari.local.md`** oluştur (`.gitignore`'da, push edilmez):

```markdown
# Cihaz notları — GİZLİ, commit edilmez

Port          : COM8
MAC           : 10:20:ba:46:52:f0
board/uuid    : 9d7607fb-55b8-4302-90ad-f8c01892db0b   (orijinal, geri yüklendi)
Geçici UUID   : 6f54949a-67d1-4882-87bf-3b0086440358   (NVS silinince oluştu — konsolda
                ikinci cihaz olarak görünüyorsa sil)
WiFi          : Pars / Welcome Baby AI

Yedekler (C:\xiaozhi):
  kritik-yedek.bin   65536 byte   bootloader + partition + NVS + otadata
  nvs-only.bin       16384 byte   sadece NVS — her flash sonrası bunu geri yaz
  stok-turkce.bin                 yamasız Türkçe firmware (geri dönüş)
```

---

## 5. Klasör düzeni

İki klasör tut, karışmasın:

```
C:\xiaozhi-proje\     ← kaynak kod, git, Claude burada çalışır
C:\xiaozhi\           ← esptool.exe, yedekler, indirdiğin .bin dosyaları
```

`flash.ps1`'i `C:\xiaozhi` klasörüne kopyala (esptool ile aynı yerde olması gerekiyor).
Kod değişince Claude'un ürettiği yeni sürümü oraya tekrar kopyalarsın — ya da tek seferlik:

```powershell
# C:\xiaozhi icinde, projedeki betige kisayol
New-Item -ItemType SymbolicLink -Path C:\xiaozhi\flash.ps1 -Target C:\xiaozhi-proje\flash.ps1
```

(Yönetici PowerShell gerekir. Zahmetse elle kopyalaman da olur.)

PowerShell betik çalıştırmayı bir kez izinli yap:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

---

## 6. Rutin iş akışı

**VS Code'da Claude'a söyle:**

> "Ekranı yukarı kaydırınca açılan bir ayar paneli ekle: ses ve parlaklık slider'ı, tema anahtarı."

Claude `CLAUDE.md`'yi okur, kaynağı inceler, kodu yazar, commit'ler, push'lar.

**Sonra sen:**

1. https://github.com/ccorayy/xiaozhi-esp32/actions → yeni çalışmayı bekle (~6 dk)
2. Yeşil tik → en altta **Artifacts** → indir → zip'ten `merged-binary.bin`'i `C:\xiaozhi`'ye çıkart
3. `C:\xiaozhi` klasöründe PowerShell:
   ```powershell
   .\flash.ps1
   ```
4. USB'yi çıkar tak, test et
5. Claude'a sonucu söyle — çalıştı mı, ekranda ne göründü, log ne dedi

**Sorun çıkarsa geri dönüş:**

```powershell
.\flash.ps1 -Firmware stok-turkce.bin
```

---

## 7. Faydalı komutlar

```powershell
# seri log izle (butona basinca ne yaziyor gormek icin)
$p = New-Object System.IO.Ports.SerialPort COM8,115200,None,8,one
$p.Open()
while ($true) { if ($p.BytesToRead -gt 0) { Write-Host -NoNewline $p.ReadExisting() }; Start-Sleep -Milliseconds 50 }
# cikis: Ctrl+C

# upstream guncellemelerini al
git fetch upstream 2>$null || git remote add upstream https://github.com/78/xiaozhi-esp32.git
git fetch upstream
git rebase upstream/main
git push --force-with-lease
```

---

## 8. Claude'a ilk mesajın

Projeyi açtıktan sonra ilk mesaj olarak bunu at — bağlamı doğru kurar:

> `CLAUDE.md` dosyasını oku. Bu fork'ta ne yapıldığını, cihazın ne olduğunu ve yol haritasını
> anla. Sonra yol haritasındaki 1. maddeye (LVGL ayar paneli) başlayalım. Önce ne yapacağını
> anlat, kod yazmadan önce onayımı al.

---

## Aklında bulunsun

- **Flash sonrası NVS geri yüklemeyi asla atlama** — `flash.ps1` bunu senin için yapıyor, elle komut yazmaya gerek yok
- **`erase-flash` kullanma** — NVS'i siler
- **Cihaz yazılımla bozulmaz** — ESP32-S3'ün ROM bootloader'ı silinemez. En kötü durumda BOOT butonuna basılı tutarak USB'yi tak, zorla indirme moduna girer
- **Yedeklerin duruyor** — `kritik-yedek.bin` her şeyi geri getirir
