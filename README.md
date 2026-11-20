# Anlık Klip

Windows 10/11 için hafif, tamamen native (C++17 / Win32 / Direct3D 11 / DXGI / Media Foundation) anlık klip kaydedici (instant replay) uygulaması. Arka planda düşük RAM kullanımıyla sürekli tampon tutar ve tek tuşla son anları MP4 olarak kaydeder.

## İndirme

- **PC (Windows 10/11):** [Releases](https://github.com/Aphelioxe/anlik-klip/releases) sayfasından `AnlikKlip.exe` dosyasını indirip doğrudan çalıştırabilirsin. Kurulum gerektirmez.
- **Android (Telefon):** Aynı [Releases](https://github.com/Aphelioxe/anlik-klip/releases) sayfasından `AnlikKlip.apk` dosyasını telefonuna indirip kurabilirsin.
- **Kaynak koddan derleme:** aşağıdaki [Derleme](#derleme) bölümüne bak.

## Özellikler

- **Sürekli RAM tamponu (ring buffer):** DXGI Desktop Duplication ve D3D11 Video Processor ile ekran yakalama; Media Foundation üzerinden H.264 video + AAC ses içeren MP4 kayıt.
- **Klip süresi seçenekleri:** Son 30 sn, 1 dk (60 sn — varsayılan), 2 dk, 3 dk veya 5 dk.
- **Çözünürlük ve FPS profilleri:** Tek tık `720p HD` / `1080p Full HD` seçimiyle `1280x720 @ 60 FPS` (varsayılan), `1280x720 @ 30 FPS`, `1920x1080 @ 60 FPS`, `1920x1080 @ 30 FPS`.
- **Bit hızı seçenekleri:** `4 Mbps`, `6 Mbps` (varsayılan), `8 Mbps`, `10 Mbps`, `12 Mbps` (`1080p60` önerilen), `15 Mbps`.
- **Çift ses kanalı ve gürültü kapısı:** Oyun / sistem sesi (WASAPI loopback) + mikrofon yakalama, ayrı ses seviyesi (`%0`–`%200`), mikrofon cihaz seçimi, mikrofon susturma ve klavye/arka plan sesini kesen ayarlanabilir **Mikrofon Gürültü Kapısı** (`-60` – `-15 dB` eşik + canlı seviye ölçer).
- **Oyun içi kısayol algılama:** Çift katmanlı algılama (RawInput + Low-Level Keyboard Hook + `RegisterHotKey` yedeği) ile klip alma (varsayılan `F9`) ve isteğe bağlı mikrofon susturma kısayolu. Yönetici yetkisiyle çalışan oyunlar için uygulama içinden **Yönetici Başlat** seçeneği mevcuttur.
- **Çok kanallı ses (Multi-track MP4):** Her klip içine **Track 1 (Miks)**, **Track 2 (Oyun / PC Sesi)** ve **Track 3 (Mikrofon)** ayrı AAC ses izleri olarak kaydedilir.
- **Oyun / uygulama adıyla otomatik etiketleme & köşe bildirimi:** Klip alındığı anda aktif pencere/oyunu (`Valorant`, `LeagueOfLegends`, `CS2`, `osu`, `Chrome`, `Masaustu` vb.) algılayıp dosya adına ekler; oyun sırasında odak çalmayan hafif köşe bildirimi (`✓ Klip Kaydedildi`) gösterir.
- **Klip galerisi, tam ekran oynatıcı ve çok kanallı editör:** Küçük resimli klip listesi, isimle canlı arama, favori yıldızı (`★`), dahili tam ekran (`⛶`, `F`/`F11`, çift tıkla tam ekran, `Esc` çıkış) Media Foundation oynatıcı. Oynatıcıdaki **✂ Düzenle** paneliyle zaman çizelgesi üzerinde başlangıç/bitiş kırpabilir, **✂ Ortadan Kes** ile aradan istenmeyen bölümü çıkarabilir, **+ Klip Ekle** ile başka bir klibi ekleyebilir, **Oyun/PC ve Mikrofon sesini bağımsız** ayarlayıp susturabilir, **9:16 Dikey** (Shorts/Reels) veya tek tık **Discord ≤10 MB / ≤25 MB** sıkıştırma ile dışa aktarabilirsin.
- **Telefon Aktarımı (Wi-Fi + Android APK):** İsteğe bağlı yerel Wi-Fi sunucusu (kapalıyken `0` ekstra RAM / açık port) ve QR kod ile eşleşen `AnlikKlip.apk` Android uygulaması. Aynı Wi-Fi ağında PC'deki klipleri yatay tam ekran oynatıcıda (çift dokunma `±5 sn`, hız kontrolü) izleyebilir, **10 MB Paylaş** ile doğrudan Discord/WhatsApp'a sıkıştırıp gönderebilir, telefondan kırpıp (`✂ Kırp`) veya kartlara basılı tutarak çoklu seçimle tek seferde telefon galerisine (`Movies/AnlikKlip`) indirebilirsin.
- **Sistem tepsisi & düşük bellek:** Pencere kapatıldığında sistem tepsisinde çalışmaya devam eder; isteğe bağlı Windows açılışında arka planda başlatma ve klip alma bildirim sesi bulunur.

## Hızlı Başlangıç

1. `AnlikKlip.exe` dosyasını boş bir klasöre koyup çalıştır. İlk açılışta yanına otomatik olarak `settings.ini`, `diag.log` ve `klipler\` klasörü oluşturulur.
2. Varsayılan klip alma tuşu `F9`'dur (**Ayarlar** sekmesinden değiştirebilirsin).
3. **Kliplerim** sekmesinden kaydedilen klipleri doğrudan uygulama içinde izleyebilir, adını değiştirebilir veya klasörde açabilirsin.

## Telefon Aktarımı (Android APK)

1. PC'de **Ayarlar** sekmesinden **Telefon Aktarımı (Wi-Fi)** kutucuğunu aç ve **QR Kod / Bağlan** butonuna tıkla.
2. Telefona `AnlikKlip.apk` dosyasını kur (`minSdk 26` / Android 8.0+).
3. Aynı Wi-Fi ağındayken telefon uygulamasından **QR Tara** ile PC ekranındaki QR kodu okut (veya **Wi-Fi ile PC Bul** + 6 haneli PIN ile bağlan).
4. Klipleri telefondan izleyebilir, **Galeriye İndir** ile `Movies/AnlikKlip` klasörüne kaydedebilir veya **Oto İndir** anahtarını açabilirsin.

## Derleme

Gereksinimler: `llvm-mingw` (`clang++.exe` ve `x86_64-w64-mingw32-windres.exe`).

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Derleme sonucunda proje kökünde `AnlikKlip.exe` oluşturulur.

## Lisans

[MIT](LICENSE)
