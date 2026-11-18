# Anlık Klip

Windows 10/11 için hafif, tamamen native (C++17 / Win32 / Direct3D 11 / DXGI / Media Foundation) anlık klip kaydedici (instant replay) uygulaması. Arka planda düşük RAM kullanımıyla sürekli tampon tutar ve tek tuşla son anları MP4 olarak kaydeder.

## İndirme

- **Hazır sürüm:** [Releases](https://github.com/Aphelioxe/anlik-klip/releases) sayfasından `AnlikKlip.exe` dosyasını indirip doğrudan çalıştırabilirsin. Kurulum gerektirmez.
- **Kaynak koddan derleme:** aşağıdaki [Derleme](#derleme) bölümüne bak.

## Özellikler

- **Sürekli RAM tamponu (ring buffer):** DXGI Desktop Duplication ve D3D11 Video Processor ile ekran yakalama; Media Foundation üzerinden H.264 video + AAC ses içeren MP4 kayıt.
- **Klip süresi seçenekleri:** Son 30 sn, 1 dk (60 sn — varsayılan), 2 dk, 3 dk veya 5 dk.
- **Çözünürlük ve FPS profilleri:** `1280x720 @ 60 FPS` (varsayılan), `1280x720 @ 30 FPS`, `1920x1080 @ 60 FPS`, `1920x1080 @ 30 FPS`.
- **Bit hızı seçenekleri:** `4 Mbps`, `6 Mbps` (varsayılan), `8 Mbps`, `10 Mbps`.
- **Çift ses kanalı ve gürültü kapısı:** Oyun / sistem sesi (WASAPI loopback) + mikrofon yakalama, ayrı ses seviyesi (`%0`–`%200`), mikrofon cihaz seçimi, mikrofon susturma ve klavye/arka plan sesini kesen ayarlanabilir **Mikrofon Gürültü Kapısı** (`-60` – `-15 dB` eşik + canlı seviye ölçer).
- **Oyun içi kısayol algılama:** Çift katmanlı algılama (RawInput + Low-Level Keyboard Hook + `RegisterHotKey` yedeği) ile klip alma (varsayılan `F9`) ve isteğe bağlı mikrofon susturma kısayolu. Yönetici yetkisiyle çalışan oyunlar için uygulama içinden **Yönetici Başlat** seçeneği mevcuttur.
- **Klip galerisi, favoriler, arama ve hızlı kırpma (trim):** Küçük resimli klip listesi, isimle canlı arama, favori yıldızı (`★` + Favoriler görünümü), yeniden adlandırma, klasörde açma, Çöp Kutusu'na silme ve uygulama içi Media Foundation oynatıcı (`Space` oynat/duraklat, `←` / `→` 5 sn ileri/geri sar, `Esc` geri, hız ve ses kontrolü). Oynatıcıdaki **✂ Kırp** aracıyla seçili aralığı **Yeni Kopya Kaydet** veya **Ana Klip Yap** (eski kopyayı Çöp Kutusu'na alarak üzerine yaz) seçenekleriyle kırpabilirsin.
- **Sistem tepsisi & düşük bellek:** Pencere kapatıldığında sistem tepsisinde çalışmaya devam eder; isteğe bağlı Windows açılışında arka planda başlatma ve klip alma bildirim sesi bulunur.

## Hızlı Başlangıç

1. `AnlikKlip.exe` dosyasını boş bir klasöre koyup çalıştır. İlk açılışta yanına otomatik olarak `settings.ini`, `diag.log` ve `klipler\` klasörü oluşturulur.
2. Varsayılan klip alma tuşu `F9`'dur (**Ayarlar** sekmesinden değiştirebilirsin).
3. **Kliplerim** sekmesinden kaydedilen klipleri doğrudan uygulama içinde izleyebilir, adını değiştirebilir veya klasörde açabilirsin.

## Derleme

Gereksinimler: `llvm-mingw` (`clang++.exe` ve `x86_64-w64-mingw32-windres.exe`).

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Derleme sonucunda proje kökünde `AnlikKlip.exe` oluşturulur.

## Lisans

[MIT](LICENSE)
