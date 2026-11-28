# Anlık Klip

[![Platform](https://img.shields.io/badge/Windows-10%20%2F%2011-0078D4?style=flat-square&logo=windows)](https://github.com/Aphelioxe/anlik-klip/releases)
[![Android](https://img.shields.io/badge/Android-8.0%2B%20(APK)-3DDC84?style=flat-square&logo=android&logoColor=white)](https://github.com/Aphelioxe/anlik-klip/releases)
[![C++17](https://img.shields.io/badge/C%2B%2B17-DXGI%20%2B%20Media%20Foundation-6366F1?style=flat-square)](build.ps1)
[![License: MIT](https://img.shields.io/badge/Lisans-MIT-22C55E?style=flat-square)](LICENSE)

**Anlık Klip**, Windows 10/11 için tamamen native (**C++17 / Win32 / Direct3D 11 / DXGI / Media Foundation**) olarak geliştirilmiş hafif bir anlık klip kaydedici (*instant replay*), çok kanallı montaj stüdyosu ve yerel Wi-Fi telefon aktarım aracıdır.

Arka planda minimal RAM ve CPU kullanımıyla sürekli döngüsel tampon tutar; tek bir kısayol tuşuyla son saniyeleri veya dakikaları MP4 olarak kaydeder.

---

## İndirme ve Kurulum

- **PC (Windows 10/11):** [Releases](https://github.com/Aphelioxe/anlik-klip/releases/latest) sayfasından `AnlikKlip.exe` dosyasını indirip doğrudan çalıştırın. Kurulum gerektirmez (taşınabilir tek dosya).
- **Android (Telefon):** Aynı [Releases](https://github.com/Aphelioxe/anlik-klip/releases/latest) sayfasından `AnlikKlip.apk` (Android 8.0+ / API 26+) dosyasını telefonunuza kurabilirsiniz.
- **Kaynak koddan derleme:** [Derleme](#kaynak-koddan-derleme) bölümüne bakın.

### Hızlı Başlangıç

1. `AnlikKlip.exe` dosyasını bir klasöre koyup çalıştırın (ilk açılışta yanında `settings.ini`, `diag.log` ve `klipler\` klasörü otomatik oluşturulur).
2. Varsayılan klip alma tuşu **`F9`**'dur (**Ayarlar** sekmesinden istediğiniz tuş veya tuş kombinasyonunu atayabilirsiniz).
3. Pencereyi kapattığınızda uygulama **sistem tepsisinde** en düşük kaynak tüketimiyle çalışmaya devam eder.

---

## Özellikler

### Arka Plan Kaydı ve Performans
- **Donanım dostu yakalama:** DXGI Desktop Duplication + Direct3D 11 Video Processor ile ekran yakalama ve Media Foundation üzerinden H.264 / AAC MP4 yazımı.
- **Seçilebilir tampon süresi:** Son `30 sn`, `1 dk` *(varsayılan)*, `2 dk`, `3 dk` veya `5 dk`.
- **Çözünürlük ve akıcılık profilleri:** Tek tık `720p HD` / `1080p Full HD` seçimi ile `1280x720 @ 60 FPS` *(varsayılan)*, `1280x720 @ 30 FPS`, `1920x1080 @ 60 FPS`, `1920x1080 @ 30 FPS`.
- **Bit hızı seçenekleri:** `4 Mbps`, `6 Mbps` *(varsayılan)*, `8 Mbps`, `10 Mbps`, `12 Mbps` (`1080p60` önerilen), `15 Mbps`.
- **Otomatik oyun/uygulama etiketleme & köşe bildirimi:** Klip kaydedildiğinde aktif pencereyi (`Valorant`, `LeagueOfLegends`, `CS2`, `osu`, `Chrome`, `Masaustu` vb.) dosya adına ekler; oyun sırasında odak çalmayan hafif köşe bildirimi gösterir.

### Ses Kanalları ve Oyun İçi Kısayollar
- **Çok izli MP4 ses kaydı:** Her klip içine ayrı kanallar kaydedilir:
  - **Track 1:** Miks *(Oyun/PC + Mikrofon — paylaşım ve harici oynatıcılar için varsayılan iz)*
  - **Track 2:** Oyun / Sistem Sesi *(Spotify arka plan müziği dahil)*
  - **Track 3:** Mikrofon
- **Gelişmiş mikrofon kontrolü:** `%0`–`%200` ayrı oyun ve mikrofon ses düzeyi, mikrofon cihaz seçimi, klavye/oda gürültüsünü kesen **Mikrofon Gürültü Kapısı** (`-60 dB` – `-15 dB` eşik + canlı dB ölçer) ve özel mikrofon susturma kısayolu.
- **Çift katmanlı kısayol algılama:** RawInput + Low-Level Keyboard Hook + `RegisterHotKey` yedeğiyle tam ekran/borderless oyunlarda güvenilir tetikleme; yönetici yetkisiyle çalışan oyunlar için Ayarlar içinde **Yönetici Başlat** desteği.

### Klip Galerisi, Oynatıcı ve Tek Tık Link Paylaşımı
- **Klip kütüphanesi:** Küçük resimli ızgara görünümü, dosya adına göre anlık arama, favori yıldızı (`★`), yeniden adlandırma, klasörde açma ve geri dönüşüm kutusuna silme.
- **Dahili oynatıcı ve hızlı düzenleme:** `0.5x`, `1.0x`, `1.5x`, `2.0x` oynatma hızı, tam ekran modu ve **✂ Düzenle** paneli (başlangıç/bitiş kırpma, **Ortadan Kes**, **+ Klip Ekle**, bağımsız oyun/mikrofon ses ayarı).
- **Tek tık Link Al:** Klipleri tek tıkla yükleyip bağlantıyı panoya kopyalar (`≤ 195 MB` klipler için doğrudan tarayıcı/Discord önizlemeli **Catbox.moe**, daha büyük dosyalar için **Gofile.io**).

### Çok Kanallı Montaj Stüdyosu (`V2` / `V1` / `A1` / `M1`)
- **Çok katmanlı zaman çizelgesi:** Ana video (**V1**), Üst Katman / Picture-in-Picture (**V2**), dalga formlu Oyun + Mikrofon sesi (**A1**) ve harici müzik (**M1**: `.mp3`, `.wav`, `.m4a`, `.aac`, `.flac`).
- **Geniş ölçekli gezinme:** Zaman çizelgesinde `60+` dakikaya kadar uzaklaştırma (`Ctrl + Tekerlek` veya yakınlaştır/uzaklaştır/sığdır butonları) ve klipleri sürükleyerek aralarına boşluk bırakma.
- **Üst Katman (V2 / PiP) & Zamanlı Çerçeve (V1):** İki klibi aynı anda veya istenen saniyeden itibaren oynatma, sahnede köşelerden tutarak canlı boyutlandırma ve taşıma.
- **5 Yerleştirilebilir Geçiş / Efekt:** **Beyaz Flaş**, **Zoom Darbe**, **Glitch / Sars**, **Karartma** ve **Perde Geçişi**; zaman çizelgesinde sürükleyip istenen saniyeye yerleştirilebilir (ayrıca klipler arası otomatik karartma seçeneği).
- **Esnek çıktı:** Klip bazlı `0.5x–2.0x` hız, ayrı oyun/mikrofon seviyesi, **`16:9 Yatay`** veya **`9:16 Dikey`** *(Shorts/Reels)*, **`Tam Kalite`**, **`Discord ≤ 10 MB`** ve **`Discord ≤ 25 MB`** akıllı sıkıştırma.

### Telefon Aktarımı (Yerel Wi-Fi + Android APK)
- **Kapalıyken 0 yük:** Telefon aktarımı Ayarlar'dan açılmadıkça ağ dinlemesi yapmaz ve ek bellek tüketmez.
- **Hızlı eşleştirme:** Yerel Wi-Fi üzerinde QR kod tarama veya otomatik ağ keşfi + 6 haneli PIN doğrulaması.
- **Mobil özellikler:** Yatay tam ekran oynatma (çift dokunma `±5 sn`, oynatma hızı), telefondan video kırpma, **10 MB Paylaş**, çoklu seçimle tek seferde telefon galerisine (`Movies/AnlikKlip`) kaydetme ve otomatik indirme modu.

---

## Klavye ve Fare Kısayolları

| Alan | Kısayol | İşlev |
| :--- | :--- | :--- |
| **Oynatıcı** | `Space` | Oynat / Duraklat |
| **Oynatıcı** | `←` / `→` | `5 sn` geri / ileri sar |
| **Oynatıcı** | `F` / `F11` / Çift Tık | Tam ekran aç / kapat |
| **Oynatıcı** | `Esc` | Tam ekrandan çık veya galeriye dön |
| **Stüdyo** | `Space` | Montajı oynat / duraklat |
| **Stüdyo** | `←` / `→` *(veya `Shift + ←/→`)* | `1 sn` *(`0.2 sn`)* geri / ileri git |
| **Stüdyo** | `S` veya `Ctrl + B` | İmleç konumundan klibi böl |
| **Stüdyo** | `[` / `I` ve `]` / `O` | Seçili klibin başlangıcını / bitişini imlece kırp |
| **Stüdyo** | `Delete` | Seçili klip, müzik, efekt veya çerçeveyi kaldır |
| **Stüdyo** | `Ctrl + Tekerlek` | Zaman çizelgesini yakınlaştır / uzaklaştır |
| **Stüdyo** | `Esc` | Stüdyodan çık |

---

## Kaynak Koddan Derleme

### Windows (`AnlikKlip.exe`)
Gereksinimler: **llvm-mingw** (`clang++.exe` ve `x86_64-w64-mingw32-windres.exe`).

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Derleme sonucunda proje kök dizininde statik olarak bağlanmış `AnlikKlip.exe` oluşturulur.

### Android (`AnlikKlip.apk`)
`android/` dizini standart bir Gradle projesidir (`compileSdk 34`, `minSdk 26`):

```powershell
cd android
.\gradlew.bat assembleRelease
```

---

## Lisans

Bu proje [MIT Lisansı](LICENSE) altında lisanslanmıştır.
