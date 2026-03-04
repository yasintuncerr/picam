# PiCam — Endüstriyel Ürün Gereksinimleri & Geliştirme Notları

> Mevcut kod tabanı (`libcamera-source.cpp`, `capture.c`, `dng-writer.cpp`, `camera_controls.html`) incelenerek hazırlanmıştır.  
> Tarih: 2026-03-04

---

## 1. Mevcut Kritik Sorunların Özeti

Aşağıdaki tablo bildirilen hataları teknik bağlamıyla özetler.

| #   | Önem | Sorun                                                              | Dosya / Satır               | Endüstriyel Etki                                                   |
| --- | ---- | ------------------------------------------------------------------ | --------------------------- | ------------------------------------------------------------------ |
| 1   | 🔴   | Still çözünürlüğü `4056×3040` hardcoded                            | `libcamera-source.cpp:538`  | IMX477 dışı sensörler tamamen çalışmaz                             |
| 2   | 🔴   | `sizeimage = width * height * 2` sabit çarpan                      | `libcamera-source.cpp:533`  | Multiplanar / non-YUV formatlarda buffer taşması veya yetersizliği |
| 3   | 🔴   | Still buffer sayısı = 1                                            | `libcamera-source.cpp:749`  | Burst capture yok; eşzamanlı video+still'de frame drop riski       |
| 4   | 🟡   | DNG `BaselineExposure` eksik                                       | `dng-writer.cpp`            | Lightroom / ACR görüntüyü karanlık açar                            |
| 5   | 🟡   | DNG `CalibrationIlluminant1` eksik                                 | `dng-writer.cpp`            | `ColorMatrix1` hangi ışık koşulunda kalibre edildi bilinmiyor      |
| 6   | 🟡   | DNG `UniqueCameraModel` eksik                                      | `dng-writer.cpp:672`        | Lightroom renk profili bulunamıyor (`\todo` notu var)              |
| 7   | 🟡   | DNG `ForwardMatrix1` eksik                                         | `dng-writer.cpp`            | Adobe/Lightroom render kalitesi düşük                              |
| 8   | 🟡   | Event handling: `pipe()`                                           | `libcamera-source.cpp:977`  | `libevent` daha verimli; yüksek FPS'de CPU israfı                  |
| 9   | 🟡   | Multiplanar ikinci plane okunmuyor                                 | `libcamera-source.cpp:416`  | YUV420 gibi formatlarda renk bozukluğu                             |
| 10  | 🟡   | Kamera listeleme API'si yok                                        | `libcamera-source.cpp:1000` | Çoklu kamera seçimi programatik yapılamıyor                        |
| 11  | 🟢   | `NoiseProfile`, `BaselineSharpness`, `BaselineNoise` DNG tag eksik | `dng-writer.cpp`            | Gürültü azaltma profili uygulanamıyor                              |
| 12  | 🟢   | EXIF timezone yok                                                  | `dng-writer.cpp:885`        | Zaman damgası yanlış yorumlanabilir                                |
| 13  | 🟢   | Raw10 packing NEON optimize değil                                  | `dng-writer.cpp:312`        | Raw12 NEON var, Raw10 yavaş                                        |
| 14  | 🟢   | UVC: V4L2 cihaz dışarıya expose                                    | `uvc.c:445`                 | Yalıtım ihlali                                                     |

---

## 2. Endüstriyel Ürün Gereksinimleri

### 2.1 Güvenilirlik & Hata Toleransı

| Gereksinim                  | Açıklama                                                                                                                       | Öncelik |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------- |
| **Watchdog entegrasyonu**   | `systemd` `WatchdogSec=` ile servis takibi; donma durumunda otomatik restart                                                   | 🔴      |
| **Yapısal hata yönetimi**   | libcamera, TIFF, mmap hatalarında `errno` tabanlı yapısal geri bildirim (şu an `stderr`'e yazılıyor)                           | 🔴      |
| **Graceful shutdown**       | `SIGTERM` / `SIGINT` alındığında capture devam ediyorsa tamamlanıp kapanmalı                                                   | 🔴      |
| **Memory leak tespiti**     | Detached thread (`libcamera-source.cpp:457`) sonrası DNG buffer serbest bırakma garanti altına alınmalı                        | 🟡      |
| **Capture timeout**         | HTTP isteği `still_capture_ready_cb` callback'ini beklediğinde sonsuz bekleme riski var; `pthread_cond_timedwait` kullanılmalı | 🔴      |
| **Çoklu istemci kilitleme** | Eşzamanlı `/capture` istekleri için mutex koruması (`capture_in_progress` flag eklendi ama HTTP katmanı korumuyor)             | 🟡      |

### 2.2 API & Protokol

| Gereksinim                 | Açıklama                                                                                                    | Öncelik |
| -------------------------- | ----------------------------------------------------------------------------------------------------------- | ------- |
| **RESTful endpointler**    | Mevcut tüm endpointler `GET` olmak üzere state-changing işlemler için `POST/PUT` kullanılmalı               | 🟡      |
| **POST body desteği**      | Capture parametreleri URL query string yerine JSON body alabilmeli (uzun profil isimleri, özel karakterler) | 🟡      |
| **Kamera listeleme**       | `GET /cameras` → mevcut kameraları ID + model ile listele                                                   | 🔴      |
| **Kamera seçimi**          | `POST /select_camera?id=X` → çoklu kamera desteği                                                           | 🔴      |
| **Capabilities endpointi** | `GET /capabilities` → sensör çözünürlüğü, FPS aralığı, desteklenen formatlar                                | 🟡      |
| **Streaming metadata**     | `/stream` yanıtında JPEG header benzeri metadata gömme (timestamp, exposure, gain)                          | 🟢      |
| **OpenAPI / Swagger**      | API'nin makine okunabilir dokümantasyonu → otomasyon araçları entegrasyon yapar                             | 🟡      |
| **Webhook desteği**        | Capture tamamlandığında dış sisteme `POST` atabilme                                                         | 🟢      |

### 2.3 Güvenlik

| Gereksinim                     | Açıklama                                                                                                                                | Öncelik |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------- | ------- |
| **mTLS (İstemci sertifikası)** | Mevcut SSL sunucu taraflı; endüstriyel ortamda istemci de doğrulanmalı                                                                  | 🟡      |
| **API key / Token**            | Bearer token veya API key ile endpoint koruması                                                                                         | 🔴      |
| **Rate limiting**              | `/capture` endpoint'i flood'a karşı korunmalı (anlık DDOS → sistem donması)                                                             | 🟡      |
| **Audit log**                  | Hangi istemci, ne zaman, hangi parametrelerle capture aldı → `syslog` veya ayrı dosyaya                                                 | 🟡      |
| **Input sanitization**         | `find_query_param` + `strtol/strtof` kombinasyonu genel olarak güvenli; ancak `name` parametresi için path traversal kontrolü eklenmeli | 🟡      |
| **CORS kısıtlama**             | `Access-Control-Allow-Origin: *` üretimde daraltılmalı                                                                                  | 🟡      |

### 2.4 Loglama & Gözlemlenebilirlik

| Gereksinim             | Açıklama                                                                                 | Öncelik |
| ---------------------- | ---------------------------------------------------------------------------------------- | ------- |
| **Yapısal loglama**    | `stderr` yerine JSON log (timestamp, level, module, message)                             | 🟡      |
| **Capture metrikleri** | Capture süresi (ms), buffer boyutu, FPS, drop sayısı Prometheus/statsd'e aktarılabilmeli | 🟢      |
| **Health endpoint**    | `GET /health` → sistem durumu, uptime, kamera bağlantısı, son hata                       | 🔴      |
| **Log rotation**       | systemd journald veya logrotate entegrasyonu                                             | 🟡      |

### 2.5 Yapılandırma Yönetimi

| Gereksinim                         | Açıklama                                                                             | Öncelik |
| ---------------------------------- | ------------------------------------------------------------------------------------ | ------- |
| **Yapılandırma dosyası**           | `picam.conf` (INI/TOML/YAML) — port, SSL yolları, profil yolu, varsayılan çözünürlük | 🔴      |
| **Ortam değişkeni desteği**        | `PICAM_PORT`, `PICAM_PROFILE_PATH` vs.                                               | 🟡      |
| **Çalışma-zamanı yeniden yükleme** | `SIGHUP` ile konfigürasyonu yeniden okuma (SSL yenileme vb.)                         | 🟢      |
| **Sensör otomatik algılama**       | Mevcut `4056×3040` hardcode yerine libcamera property'lerinden okuma                 | 🔴      |

### 2.6 Test & CI/CD

| Gereksinim              | Açıklama                                                                          | Öncelik |
| ----------------------- | --------------------------------------------------------------------------------- | ------- |
| **Unit test altyapısı** | `capture_controls`, DNG tag mantığı, query parser için testler                    | 🟡      |
| **Mock kamera**         | libcamera `IPAProxy` veya v4l2loopback üzerinden CI'da gerçek sensör olmadan test | 🟡      |
| **Integration test**    | Headless `curl` tabanlı API smoke test scripti                                    | 🟡      |
| **Otomatik derleme**    | GitHub Actions / GitLab CI pipeline (cross-compile ARM)                           | 🟢      |

---

## 3. Otomasyon Gereksinimleri

### 3.1 Capture Otomasyon API'si

```json
POST /capture/batch
{
  "count": 10,
  "interval_ms": 500,
  "profile": "fluorescence_40x",
  "output_dir": "/mnt/nas/slides/001",
  "format": "dng"
}
```

| Özellik                          | Detay                                                                               | Öncelik      |
| -------------------------------- | ----------------------------------------------------------------------------------- | ------------ |
| **Batch capture**                | N adet görüntü, belirli aralıklarla arka planda çek                                 | 🔴           |
| **Zamanlayıcı tabanlı capture**  | `cron` benzeri tetikleyici veya Unix socket üzerinden komut                         | 🟡           |
| **Harici tetikleyici (trigger)** | GPIO pin üzerinden capture başlatma (Raspberry Pi GPIO → libgpiod)                  | 🔴 Mikroskop |
| **Sekans scripting**             | JSON veya YAML tabanlı sekans tanımı: `{ adım: pozlama, bekleme, çekim, ilerleme }` | 🟡           |
| **Dosya adı şablonu**            | `{date}_{time}_{profile}_{index}.dng` biçiminde yapılandırılabilir dosya adı        | 🟡           |
| **Capture queue**                | İstekler kuyruğa alınır; birden fazla istemci aynı anda isteyebilir                 | 🟡           |

### 3.2 Uzaktan Kontrol

| Özellik             | Detay                                                                | Öncelik |
| ------------------- | -------------------------------------------------------------------- | ------- |
| **WebSocket / SSE** | Kamera durumu (exposure, gain, FPS) gerçek zamanlı push              | 🟡      |
| **MQTT pub/sub**    | Endüstriyel IoT entegrasyonu; capture triggering ve status reporting | 🟢      |
| **gRPC arayüzü**    | Yüksek hızlı otomasyon için binary protokol                          | 🟢      |
| **Python SDK**      | `picam_client.py` — capture, profil yönetimi, batch işlem            | 🟡      |
| **CLI aracı**       | `picam-cli capture --profile fluorescence --count 5`                 | 🟡      |

### 3.3 Depolama Yönetimi

| Özellik                        | Detay                                                       | Öncelik     |
| ------------------------------ | ----------------------------------------------------------- | ----------- |
| **NAS / SFTP transfer**        | Capture sonrası otomatik uzak depolamaya gönderme           | 🟡          |
| **Disk doluluk izleme**        | Disk %X dolduğunda uyarı veya capture durdurma              | 🔴          |
| **Dosya adı çakışma yönetimi** | UUID veya monoton sıra numarası + timestamp                 | 🟡          |
| **Checksum doğrulama**         | SHA-256 ile dosya bütünlüğü (tıbbi görüntüleme zorunluluğu) | 🔴 Standart |

---

## 4. Mikroskop Kullanımı İçin Özel Gereksinimler

### 4.1 Aydınlatma Kontrolü

| Gereksinim                     | Açıklama                                                        | Öncelik |
| ------------------------------ | --------------------------------------------------------------- | ------- |
| **LED şiddeti kontrolü**       | I²C / PWM üzerinden LED sürücü entegrasyonu (Köhler aydınlatma) | 🔴      |
| **Dalga boyu seçimi**          | RGB/multispektral LED için kanal bazlı açma/kapama              | 🔴      |
| **Aydınlatma profili**         | Görüntü profiline bağlı aydınlatma ayarı otomatik uygulanmalı   | 🔴      |
| **Floresan/faz kontrast modu** | Farklı optik modlar için hazır aydınlatma presetleri            | 🟡      |

### 4.2 Odaklama (Focus)

| Gereksinim                                | Açıklama                                                                           | Öncelik |
| ----------------------------------------- | ---------------------------------------------------------------------------------- | ------- |
| **Yazılım tabanlı odak kalitesi metriği** | Laplacian varyansı veya Brenner gradient → `/focus_quality` endpoint'i             | 🔴      |
| **Otomatik odak (autofocus)**             | Z-stack + odak metrikleri ile tepe noktası arama                                   | 🟡      |
| **Z-stack capture**                       | `POST /zstack { z_start, z_end, z_step, profile }` → TIFF stack veya ayrı DNG seti | 🔴      |
| **Motorlu odak entegrasyonu**             | Stepper motor / piezo kontrolü (UART/USB-Serial → `GET /focus/move?delta=100`)     | 🟡      |

### 4.3 Sahne (Stage) Kontrolü

| Gereksinim                     | Açıklama                                                                        | Öncelik |
| ------------------------------ | ------------------------------------------------------------------------------- | ------- |
| **XY stage API**               | `POST /stage/move { dx_um, dy_um }`                                             | 🟡      |
| **Tile capture (mozaik)**      | `POST /tile_scan { rows, cols, overlap_pct, profile }` → Grid çekim + stitching | 🔴      |
| **Pozisyon geri bildirimi**    | Enkoder okuma → `/stage/position` endpointi                                     | 🟡      |
| **Home / origin kalibrasyonu** | Başlangıç noktası belirleme ve yazılımsal limit                                 | 🟡      |

### 4.4 Görüntü Kalitesi & Kalibrasyon

| Gereksinim                  | Açıklama                                                                       | Öncelik |
| --------------------------- | ------------------------------------------------------------------------------ | ------- |
| **Flatfield düzeltme**      | Aydınlatma düzensizliklerini gidermek için referans görüntü çıkarma            | 🔴      |
| **Darkfield düzeltme**      | Sensör dark frame kalibrasyonu (uzun pozlamalarda kritik)                      | 🔴      |
| **Renk kalibrasyon hedefi** | MacBeth / Gretag ColorChecker tabanlı `ColorMatrix` kalibrasyonu               | 🟡      |
| **Piksel kalibrasyonu**     | µm/piksel değerinin profil dosyasında saklanması ve TIFF metadata'ya yazılması | 🔴      |
| **Histogram endpointi**     | `/histogram` → canlı luma/RGB histogramı (JSON veya PNG)                       | 🟡      |
| **SNR / BRISQUE metriği**   | Capture başına otomatik kalite skoru → metadata'ya gömme                       | 🟢      |

### 4.5 Mikroskopi Metadata

| Gereksinim                         | Açıklama                                                                | Öncelik |
| ---------------------------------- | ----------------------------------------------------------------------- | ------- |
| **Objektif bilgisi**               | Büyütme (10x/40x/100x), NA → DNG XMP veya TIFF custom tag               | 🔴      |
| **Deney metadata**                 | Numune ID, operatör, deney kodu → DNG/TIFF custom tag veya sidecar JSON | 🔴      |
| **Zaman damgası (UTC + timezone)** | ISO 8601 formatında tam zaman damgası (mevcut timezone eksik)           | 🔴      |
| **Kondenser ayarı**                | Aydınlatma NA, kondenser diafram pozisyonu                              | 🟢      |

---

## 5. Uluslararası Standartlara Uyum

### 5.1 Tıbbi Cihaz Standartları

| Standart                          | Kapsam                             | Gerekli Aksiyonlar                                                                         |
| --------------------------------- | ---------------------------------- | ------------------------------------------------------------------------------------------ |
| **ISO 13485:2016**                | Tıbbi cihaz kalite yönetim sistemi | Design history file, risk analizi (ISO 14971), doğrulama & validasyon kayıtları            |
| **IEC 62304**                     | Tıbbi cihaz yazılımı yaşam döngüsü | Yazılım sınıfı belirleme (Class B/C), SOUP listesi (libcamera, libtiff), birim test kanıtı |
| **IEC 60601-1**                   | Tıbbi elektrikli cihaz güvenliği   | EMC sertifikası, elektriksel güvenlik testleri (donanım)                                   |
| **FDA 21 CFR Part 820 / Part 11** | ABD'de üretim ve elektronik kayıt  | Audit trail, imzalı kayıtlar, erişim kontrol                                               |
| **EU MDR 2017/745**               | Avrupa tıbbi cihaz yönetmeliği     | CE işareti, teknik dosya, conformity assessment                                            |

### 5.2 Görüntü Formatı Standartları

| Standart          | Kapsam                              | Gerekli Aksiyonlar                                                                                           |
| ----------------- | ----------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| **DICOM (PS3.x)** | Tıbbi görüntü arşivleme ve iletişim | DNG → DICOM dönüştürücü; Whole Slide Imaging SOP (`1.2.840.10008.5.1.4.1.1.77.1.6`)                          |
| **OME-TIFF**      | Açık mikroskopi standardı           | OME-XML metadata gömme; çoklu kanal, Z-stack, time-lapse desteği                                             |
| **BigTIFF**       | >4GB dosya desteği                  | Tile scan / Z-stack için zorunlu                                                                             |
| **JPEG 2000**     | Kayıpsız sıkıştırma                 | DICOM iletimi için tercih edilen codec                                                                       |
| **DNG 1.6**       | Adobe ham görüntü standardı         | `UniqueCameraModel`, `ForwardMatrix1`, `BaselineExposure`, `CalibrationIlluminant1` tag'larının tamamlanması |

### 5.3 Ağ & Güvenlik Standartları

| Standart      | Kapsam                      | Gerekli Aksiyonlar                                                            |
| ------------- | --------------------------- | ----------------------------------------------------------------------------- |
| **IEC 62443** | Endüstriyel siber güvenlik  | Güvenlik zoning, authentication, audit logging                                |
| **HIPAA**     | ABD sağlık verisi gizliliği | Görüntü şifreleme, erişim kaydı, veri silme politikası                        |
| **ISO 27001** | Bilgi güvenliği yönetimi    | Risk değerlendirmesi, erişim kontrolü                                         |
| **TLS 1.3**   | Güvenli iletişim            | Mevcut SSL; cipher suite kısıtlanmalı, sertifika rotasyonu otomatize edilmeli |

### 5.4 Kalite & Test Standartları

| Standart          | Gerekli Aksiyonlar                                                                    |
| ----------------- | ------------------------------------------------------------------------------------- |
| **ISO/IEC 25010** | Yazılım kalite özellikleri → güvenilirlik, sürdürülebilirlik metrikleri belirlenmeli  |
| **ISO 15739**     | Görüntü gürültüsü ölçüm → `NoiseProfile` DNG tag'ının kalibre edilmesi                |
| **ASTM E2990**    | Dijital patoloji görüntü kalitesi; odak kalitesi, renk doğruluğu, çözünürlük testleri |

---

## 6. Geliştirme Yol Haritası (Öncelik Sırasına Göre)

```
Faz 1 — Kritik Stabilite (1-2 ay)
├── [🔴] Sensör çözünürlüğü dinamik okunması
├── [🔴] sizeimage hesabı düzeltmesi
├── [🔴] Capture timeout (pthread_cond_timedwait)
├── [🔴] /health endpoint
├── [🔴] API key authentication
└── [🔴] Disk doluluk izleme

Faz 2 — Mikroskop Entegrasyonu (2-4 ay)
├── [🔴] LED aydınlatma kontrolü (I²C/PWM)
├── [🔴] Odak kalitesi metriği (/focus_quality)
├── [🔴] Piksel kalibrasyonu (µm/px profil desteği)
├── [🔴] Flatfield/darkfield düzeltme
├── [🔴] Tile scan / mozaik capture
├── [🟡] Z-stack capture
└── [🟡] Motorlu odak entegrasyonu

Faz 3 — Otomasyon & Standartlar (4-8 ay)
├── [🔴] Batch capture API
├── [🔴] GPIO trigger desteği
├── [🟡] OME-TIFF export
├── [🟡] DICOM dönüştürücü
├── [🔴] DNG eksik tag'lar (ForwardMatrix1, BaselineExposure vb.)
├── [🟡] Python SDK / CLI araç
└── [🟡] OpenAPI dokümantasyonu

Faz 4 — Sertifikasyon (8-18 ay)
├── ISO 13485 kalite sistemi kurulumu
├── IEC 62304 yazılım yaşam döngüsü
├── EU MDR teknik dosya
└── DICOM uyumluluk testleri
```

---

## 7. Mimari Öneriler

### Mevcut Mimari Sorunları

```
HTTP server (capture.c)
    └── pthread per client            ← ölçeklenmiyor
    └── senkron capture wait          ← istemci bağlantısı kesilirse buffer sızdırır
    └── JSON parse: fscanf             ← kırılgan; standart JSON kütüphanesi gerekli
    └── profil persistansı: flat JSON  ← SQLite daha güvenilir
```

### Önerilen Hedef Mimari

```
picam daemon
├── libcamera backend (async, event-driven)
├── HTTP/2 server (libmicrohttpd veya mongoose)
│   ├── /api/v1/* — JSON REST
│   ├── /stream   — MJPEG/H.264
│   └── /ws       — WebSocket (status push)
├── Capture pipeline
│   ├── Queue (ring buffer, N=8)
│   ├── DNG writer (thread pool)
│   └── JPEG encoder (HW accelerated)
├── Peripheral bus
│   ├── LED controller (I²C)
│   ├── Focus motor (UART)
│   └── GPIO trigger
└── Storage backend
    ├── Local (configurable path)
    ├── NAS (SMB/NFS mount)
    └── SFTP push
```

---

_Bu belge yaşayan bir dokümandır. Her geliştirme fazından sonra güncellenmelidir._
