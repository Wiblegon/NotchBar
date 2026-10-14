# NotchBar

Windows için hafif bir görev çubuğu alternatifi (C++20, Win32, Direct2D/DirectWrite, DirectComposition, C++/WinRT).
Üç parça: ekranın üst ortasına bağlı **çentik** (saat, ses, ağ, pil, medya, bildirimler), alt ortada yalnızca açık
uygulamaları gösteren **uygulama alanı** ve Windows tuşuyla açılan **arama çubuğu / favoriler paneli**.

## Derleme

Gereken: Visual Studio 2022 Build Tools (MSVC v143, "C++ masaüstü" iş yükü), Windows SDK 10.0.22621+ (C++/WinRT
başlıkları SDK ile gelir), CMake ve Ninja (Build Tools içinde var). Dış bağımlılık yok.

```bat
build.cmd            :: Release derler ve birim testlerini (ctest) çalıştırır
build.cmd Debug
```

Çıktı: `build\Release\NotchBar.exe` (statik CRT, Visual C++ yeniden dağıtılabilir paketi gerekmez).

> Not: Proje klasörünün yolunda Türkçe/ASCII dışı karakter olmaması önerilir.

## Çalıştırma modları

| Komut | Açıklama |
|---|---|
| `NotchBar.exe` | Gerçek mod: Windows görev çubuğunu gizler, Windows tuşunu yakalar. İlk açılışta otomatik başlatmayı önerir. |
| `NotchBar.exe --preview` | Önizleme: görev çubuğuna ve başlangıç kaydına dokunmaz, ayrı veri klasörü (`%LOCALAPPDATA%\NotchBar\Preview`). |
| `--install [--quiet]` | Yönetici izni olmadan `%LOCALAPPDATA%\Programs\NotchBar`'a kurar; Başlat ve masaüstü kısayolları, "Uygulamalar ve özellikler" kaydı. |
| `--uninstall [--quiet]` | Görev çubuğunu geri getirir, başlangıç kaydını ve kısayolları siler, klasörü kaldırır. |
| `--restore-taskbar` | Windows görev çubuğunu her durumda geri getirir (masaüstünde ayrı kısayolu var). |
| `--exit` | Çalışan örneği düzgün kapatır (görev çubuğu geri gelir). |
| `--enable-autostart` / `--disable-autostart` | Kullanıcıya özel Görev Zamanlayıcı görevi (oturum açılışı, gecikmesiz, öncelik 4). |

Kullanım: Windows tuşu → arama çubuğu (yaz: uygulama, dosya, `12,5*(3+1)` gibi hesap, web). Çubuğun üzerine gelince
favoriler paneli fare hangi yönden geldiyse o tarafta açılır; dişli simgesinden ayarlara, "Windows görev çubuğunu geri
getir" ve "NotchBar'dan çık" seçeneklerine ulaşılır.

## Mimari (özet)

- `src/model` — saf, test edilen mantık: görünürlük durum makineleri, panel yönü, hesap makinesi, favoriler/klasörler,
  ayarlar (sürümlü, bozuk dosyada güvenli varsayılan), pil verisi çözümleme, tam ekran sınıflandırma, bildirim politikası.
- `src/gfx` — tek D3D11/D2D cihazı + DirectComposition yüzeyleri; yalnızca değişince/animasyon sırasında çizim, gizli
  yüzeyler GPU belleği tutmaz, cihaz kaybında yeniden oluşturma. Simge önbelleği sınırlı (LRU).
- `src/sys` — Windows entegrasyonları: pencere izleme (WinEvent), ekran modu, görev çubuğu denetimi + çökme kurtarma
  yardımcı süreci (`--watchdog`, süreç beklemeli, sorgulama yok), klavye kancası (ayrı iş parçacığı), otomatik başlatma,
  Core Audio, ağ, güç, medya oturumları, bildirimler, AppsFolder kataloğu, Windows Search dosya araması, pil sağlayıcıları.
- `src/ui` — çentik, alt alan, başlatıcı + panel, açılır kutular, bildirim kutucukları, yerel EDIT tabanlı metin alanı.

Teknik notlar:
- **Composition:** Windows.UI.Composition yerine DirectComposition seçildi; içerik zaten D2D ile çiziliyor (çentik şekli
  her animasyon karesinde yeniden çizilmeli), DispatcherQueue/WinRT compositor gerektirmiyor ve Windows 10'un tamamında var.
- **Görev çubuğu:** "Gizleme" = belgelenmiş appbar API'siyle otomatik gizlemeye alma (çalışma alanını bırakır) +
  pencereleri gizleme. Hiçbir sistem dosyası değiştirilmez, Explorer kapatılmaz. Kullanıcının asıl ayarı
  `taskbar-state.json`'a önceden yazılır; normal çıkış, çökme (watchdog) ve kaldırmada geri yüklenir. Explorer yeniden
  başlayınca `TaskbarCreated` ile yeniden uygulanır. Sınıf adları (`Shell_TrayWnd`, `Shell_SecondaryTrayWnd`)
  `TaskbarAdapter`'da. Shell Launcher yalnızca Enterprise/Education/IoT sürümlerinde var, kullanılmadı.
- **Windows tuşu:** Win basılı/bırakılmasını izleyen düşük seviyeli kanca; Win+X kombinasyonları hiç yutulmaz. Yalnız
  tek başına basılan Win'in bırakılması tutulup atanmamış bir "maske" tuşuyla (VK 0xE8) yeniden gönderilir.
- **Bildirimler:** Paketsiz uygulamada `UserNotificationListener` okunabiliyor ama `NotificationChanged` olayı paket
  kimliği olmadan çalışmıyor (0x80070490). Okuma: bildirim balonu penceresi görününce + 30 sn'lik güvenlik okuması.

## Doğrulanan durum (Windows 11 Home 10.0.26200, 1920×1080, 144 Hz, tek monitör)

- 30 birim testi geçiyor (`tests/`).
- Görev çubuğu gizleme, normal çıkışta geri yükleme, zorla sonlandırmada watchdog ile geri yükleme, Explorer yeniden
  başlaması: doğrulandı (otomatik gizleme ayarı ve çalışma alanı başlangıç değerlerine döndü).
- Win+E, Win+R, Win+Tab, Win+Shift+S, Win+D, Win+ok tuşları, Win+Ctrl+D/sol/sağ/F4 çalışıyor; tek Win başlatıcıyı
  açıyor, Başlat menüsü açılmıyor. Win+L bilinçli olarak test edilmedi (oturumu kilitler).
- Ekran modları: büyütülmüş pencere, Chrome F11, kenarlıksız tam ekran (fsprobe), masaüstü — doğru algılandı.
- Otomatik başlatma görevi: ayarlar doğrulandı, görev elle tetiklenince açılış ~282 ms (oluşturma→hazır). Gerçek oturum
  açılışından ölçüm yapılmadı.
- Boşta CPU: sabit masaüstünde %0,0065, aktif bir pencere öndeyken %0,0049 (16 mantıksal çekirdekli makinenin oranı).
  Bellek: working set ~86 MB, private ~69–72 MB, GPU özel bellek ~15 MB. Watchdog: 0 CPU, ~2 MB private.
- fsprobe (gerçek oyun değil) kare süreleri NotchBar kapalı/açık: `docs/measurements/`.
- Kurulum/kaldırma (HKCU, yönetici izni olmadan) doğrulandı.

## Doğrulanmayan / kalan işler

- **HyperX Cloud III S:** okuyucu yazıldı (açık kaynak protokol, yalnızca okuma komutları) ama alıcı takılı olmadığı
  için donanımda denenmedi. **RAWM ES21 Pro:** doğrulanmış pil yöntemi bulunamadı; cihaz yalnızca tanınır, paket gönderilmez.
- Bluetooth LE Battery Service okuyucusu donanımda denenmedi (uygun cihaz yoktu).
- Çoklu monitör, DPI değişimi, uyku/uyanma, kilitleme, GPU cihaz kaybı: kodda ele alındı, gerçek testte denenmedi.
  Arayüz yalnızca ana monitörde gösteriliyor.
- Windows 10: kullanılan API'lere göre 1809+ hedefleniyor (medya oturumları 1809, PerMonitorV2 1703), denenmedi.
- Özel çizilen yüzeyler için UI Automation sağlayıcıları yok (arama alanı yerel EDIT olduğu için erişilebilir).
- Arayüz boyutu ayarının ekranda doğrulanması, ayrıntılı teknik rapor ve kullanıcı belgesi.

## Bağımlılıklar ve lisanslar

Yalnızca Windows SDK ve MSVC (sistem bileşenleri). Üçüncü taraf kod yok. HyperX protokol bilgileri MIT lisanslı
[HeyOkay/HaloBattery](https://github.com/HeyOkay/HaloBattery) ve
[LennardKittner/HyperHeadset](https://github.com/LennardKittner/HyperHeadset) belgelerinden alındı (kod kopyalanmadı).
