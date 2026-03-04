# PiCam — Refactor Planı

> **Bütçe:** Haftada 5 saat  
> **Strateji:** Strangler Fig Pattern — mevcut sistem çalışır halde kalırken modül modül değiştirilir  
> **Kural:** Her sprint sonunda `main` branch derlenir ve temel capture çalışır

---

## Çalışma Sistemi

### Sprint Yapısı (2 Hafta = 10 saat)

```
Sprint Başlangıcı
├── Gün 1 (1h) — Review + planlama, önceki sprint retrospektifi
├── Gün 2-3 (2×1.5h = 3h) — Ana geliştirme oturumları
├── Gün 4 (1h) — Test + düzeltme
└── Gün 5 (0.5h) — Dokümantasyon güncellemesi, commit, sonraki sprintin hazırlığı
```

### Her Oturum Kuralları

1. **İlk 10 dk:** `git pull`, son değişiklikleri incele, odak belirle
2. **Ana çalışma:** tek bir modüle odaklan, kapsam dışına çıkma
3. **Son 10 dk:** çalışır build kontrolü, stash veya commit, sonraki oturuma not
4. **Commit formatı:** `refactor(modül): ne yapıldı — sprint-N`

### Definition of Done (Bir İş Kalemi Bitmiş Sayılır)

- [ ] Kod derleniyor
- [ ] Manuel smoke test geçiyor (`/capture` çalışıyor)
- [ ] Unit test varsa yeşil
- [ ] `ARCHITECTURE.md` güncellendi (gerekiyorsa)
- [ ] Commit atıldı

---

## Faz 0 — Zemin Hazırlığı (Sprint 1–2 | ~20 saat)

> **Hedef:** Sıfır işlevsel değişiklik. Refactor için güvenli zemin yarat.

### Sprint 1 (Hafta 1–2)

| #   | İş Kalemi                                                    | Süre | Dosya                         | Çıktı                        |
| --- | ------------------------------------------------------------ | ---- | ----------------------------- | ---------------------------- |
| 0.1 | Test altyapısı kur (Catch2, meson test target)               | 1.5h | `tests/meson.build`           | `meson test` çalışıyor       |
| 0.2 | CI yok → en azından `meson compile` ile GitHub Action        | 1h   | `.github/workflows/build.yml` | PR'larda derleme kontrolü    |
| 0.3 | spdlog entegrasyonu — tüm `fprintf/cerr` wrapper'a taşı      | 1.5h | `infra/Logger.h/cpp`          | Seviyeli log çıktısı         |
| 0.4 | toml++ entegrasyonu — `picam.toml` okuma (port, profil yolu) | 1h   | `infra/Config.h/cpp`          | `main.c` hardcode temizlendi |

**Sprint 1 Smoke Test:**

```bash
meson test -C build/
./build/picam --config picam.toml -p 8080 uvc.0
curl http://localhost:8080/capture -o /tmp/test.dng
```

### Sprint 2 (Hafta 3–4)

| #   | İş Kalemi                                                                                              | Süre | Dosya                      | Çıktı                        |
| --- | ------------------------------------------------------------------------------------------------------ | ---- | -------------------------- | ---------------------------- |
| 0.5 | Sensör çözünürlüğü hardcode fix (kritik bug)                                                           | 1h   | `libcamera-source.cpp:538` | IMX477 dışı kamera çalışır   |
| 0.6 | `sizeimage` hesabı libcamera'dan dinamik oku                                                           | 0.5h | `libcamera-source.cpp:533` | Buffer overflow riski kalkar |
| 0.7 | Capture timeout: `pthread_cond_timedwait` (30s)                                                        | 1h   | `capture.c`                | Sonsuz bekleme riski kalkar  |
| 0.8 | DNG eksik tag'lar: `ForwardMatrix1`, `BaselineExposure`, `CalibrationIlluminant1`, `UniqueCameraModel` | 2.5h | `dng-writer.cpp`           | Lightroom/ACR uyumlu DNG     |

**Sprint 2 Smoke Test:**

```bash
# Lightroom'da açıldığında doğru pozlama ve renk
curl http://localhost:8080/capture -o /tmp/test.dng
exiftool /tmp/test.dng | grep -E "Forward|Baseline|Calibration|UniqueCameraModel"
```

---

## Faz 1 — Core Layer Çıkarımı (Sprint 3–6 | ~40 saat)

> **Hedef:** `libcamera-source.cpp` god object'i parçala. Her modül tek bir sorumluluğa sahip olsun.

### Sprint 3 (Hafta 5–6) — Logger & Config Stabilizasyon + nlohmann/json

| #   | İş Kalemi                                                    | Süre | Dosya                                 |
| --- | ------------------------------------------------------------ | ---- | ------------------------------------- |
| 1.1 | `nlohmann/json` entegrasyonu                                 | 0.5h | `meson.build`                         |
| 1.2 | `ProfileService` sınıfı: `fscanf` → JSON (nlohmann)          | 2h   | `services/ProfileService.h/cpp`       |
| 1.3 | ProfileService unit testleri (save/load/delete)              | 1.5h | `tests/unit/test_profile_service.cpp` |
| 1.4 | `capture.c` HTTP handler'larını ProfileService'e delegate et | 1h   | `capture.c` + `api/`                  |

**Test:**

```bash
meson test -C build/ --suite unit
```

### Sprint 4 (Hafta 7–8) — ICameraBackend Interface

| #   | İş Kalemi                                                                     | Süre | Dosya                                |
| --- | ----------------------------------------------------------------------------- | ---- | ------------------------------------ |
| 1.5 | `ICameraBackend` pure interface tanımla                                       | 1h   | `core/camera/ICameraBackend.h`       |
| 1.6 | `LibcameraBackend`: mevcut `libcamera-source.cpp`'den kamera init kodunu taşı | 2h   | `core/camera/LibcameraBackend.h/cpp` |
| 1.7 | `MockCameraBackend` test için                                                 | 1h   | `tests/mocks/MockCameraBackend.h`    |
| 1.8 | Interface üzerinden smoke test                                                | 1h   | —                                    |

### Sprint 5 (Hafta 9–10) — CameraPipeline

| #    | İş Kalemi                                                                  | Süre | Dosya                              |
| ---- | -------------------------------------------------------------------------- | ---- | ---------------------------------- |
| 1.9  | `CameraPipeline` sınıfı: video stream + still capture orkestrasyonu        | 2.5h | `core/camera/CameraPipeline.h/cpp` |
| 1.10 | Capture queue (ring buffer, capacity=8)                                    | 1.5h | `core/camera/CaptureQueue.h`       |
| 1.11 | `controls_dirty_` / `restore_pending_` state → `CameraPipeline` içine taşı | 1h   | —                                  |

### Sprint 6 (Hafta 11–12) — DNG Metadata Modülü

| #    | İş Kalemi                                               | Süre | Dosya                                |
| ---- | ------------------------------------------------------- | ---- | ------------------------------------ |
| 1.12 | `DngMetadata` sınıfı: tag yönetimi object modeli        | 2h   | `core/metadata/DngMetadata.h/cpp`    |
| 1.13 | Mikroskopi metadata desteği (objektif, µm/px, deney ID) | 1.5h | `core/metadata/MicroscopyMetadata.h` |
| 1.14 | DNG metadata unit testleri                              | 1.5h | `tests/unit/test_dng_metadata.cpp`   |

---

## Faz 2 — API Katmanı (Sprint 7–9 | ~30 saat)

> **Hedef:** `capture.c`'deki raw socket server → mongoose event-driven server

### Sprint 7 (Hafta 13–14) — HTTP Server Taşıma

| #   | İş Kalemi                                        | Süre | Dosya                          |
| --- | ------------------------------------------------ | ---- | ------------------------------ |
| 2.1 | mongoose entegrasyonu (meson subproject)         | 1h   | `meson.build`                  |
| 2.2 | `/capture` ve `/camera_status` route'larını taşı | 2.5h | `api/routes/CaptureRoutes.cpp` |
| 2.3 | `/profiles/*` route'larını taşı                  | 1.5h | `api/routes/ProfileRoutes.cpp` |

**Test:**

```bash
# Eski server davranışıyla eşdeğer
curl http://localhost:8080/capture > /tmp/new.dng
curl http://localhost:8080/profiles
curl http://localhost:8080/camera_status
```

### Sprint 8 (Hafta 15–16) — HTTP Güvenlik ve Health

| #   | İş Kalemi                                                  | Süre | Dosya                             |
| --- | ---------------------------------------------------------- | ---- | --------------------------------- |
| 2.4 | `/health` endpoint                                         | 0.5h | `api/routes/StatusRoutes.cpp`     |
| 2.5 | API key middleware (`X-API-Key` header)                    | 1.5h | `api/HttpServer.cpp`              |
| 2.6 | Rate limiting (`/capture` için max 1 req/s)                | 1h   | `api/HttpServer.cpp`              |
| 2.7 | Input sanitization: path traversal kontrolü (`name` param) | 1h   | `api/routes/ProfileRoutes.cpp`    |
| 2.8 | CORS kısıtlama (konfig'den domain listesi)                 | 0.5h | `infra/Config.h`                  |
| 2.9 | Integration smoke test scripti                             | 0.5h | `tests/integration/smoke_test.sh` |

### Sprint 9 (Hafta 17–18) — WebSocket Status Push

| #    | İş Kalemi                                                  | Süre | Dosya                       |
| ---- | ---------------------------------------------------------- | ---- | --------------------------- |
| 2.10 | WebSocket endpoint `/ws` — exposure/gain push 2s'de bir    | 2.5h | `api/WebSocketServer.h/cpp` |
| 2.11 | UI güncelleme: `setInterval(pollStatus)` → WS ile değiştir | 1h   | `www/camera_controls.html`  |
| 2.12 | Graceful shutdown (SIGTERM handler)                        | 1.5h | `src/main.cpp`              |

---

### Sprint 9.5 (Hafta 19–20) — Pluggable Stream Katmanı

> **Bağlam:** Mevcut kodda stream = UVC hardcoded. Bu sprint stream'i sink'ten bağımsız hale getirir.

| #    | İş Kalemi                                                               | Süre | Dosya                                |
| ---- | ----------------------------------------------------------------------- | ---- | ------------------------------------ |
| 2.13 | `IStreamSink` pure interface + `VideoFrame` struct tanımla              | 1h   | `stream/IStreamSink.h`               |
| 2.14 | `StreamManager` sınıfı: fan-out, runtime sink ekleme/çıkarma            | 1.5h | `services/StreamManager.h/cpp`       |
| 2.15 | `UvcSink`: mevcut UVC gadget kodunu `IStreamSink` olarak sarmala        | 1.5h | `stream/UvcSink.h/cpp`               |
| 2.16 | `RtspSink` iskelet: V4L2 M2M H.264 encode → RTSP (mediamtx sidecar)     | 2.5h | `stream/RtspSink.h/cpp`              |
| 2.17 | `picam.toml`'da `[stream]` bölümü → aktif sink'ler konfig'den yüklensin | 0.5h | `infra/Config.h`                     |
| 2.18 | `/stream/status` + `/stream/start?type=rtsp` endpoint'leri              | 1h   | `api/routes/StreamRoutes.h/cpp`      |
| 2.19 | StreamManager unit testi (mock sink, fan-out doğrulaması)               | 1h   | `tests/unit/test_stream_manager.cpp` |

**Test:**

```bash
# UVC hâlâ çalışıyor mu?
curl http://localhost:8080/stream/status

# RTSP: VLC veya ffplay ile bağlan
ffplay rtsp://192.168.7.2:8554/microscope

# Sadece UDP sink etkin, UVC kapalı
curl -X POST http://localhost:8080/stream/start?type=udp
curl -X DELETE http://localhost:8080/stream/stop?type=uvc
```

> **Codec notu (Pi Zero 2W):**  
> H.264 → `/dev/video11` V4L2 M2M HW encoder (düşük CPU, 30fps)  
> H.265 → FFmpeg SW (yavaş, 15fps altı için)  
> MJPEG → libjpeg (UvcSink + UdpSink için ideal)  
> Birden fazla sink eş zamanlı etkin olabilir; her sink frame'ini bağımsız encode eder.

---

## Faz 3 — Mikroskop Özellikleri (Sprint 10–14 | ~50 saat)

> **Hedef:** Mikroskop kullanımı için kritik özellikler

### Sprint 10 — Odak Kalitesi Metriği

| #   | İş Kalemi                                                     | Süre |
| --- | ------------------------------------------------------------- | ---- |
| 3.1 | Laplacian varyansı hesaplama (JPEG thumbnail üzerinden hızlı) | 2h   |
| 3.2 | `/focus_quality` GET endpoint                                 | 1h   |
| 3.3 | Odak kalitesi unit testi (bilinen blur/sharp görüntülerle)    | 2h   |

### Sprint 11 — Flatfield / Darkfield Kalibrasyon

| #   | İş Kalemi                                               | Süre |
| --- | ------------------------------------------------------- | ---- |
| 3.4 | `POST /calibration/flatfield` — referans görüntü kaydet | 2h   |
| 3.5 | `POST /calibration/darkfield` — dark frame kaydet       | 1h   |
| 3.6 | Capture pipeline'a düzeltme uygulama                    | 2h   |

### Sprint 12 — Batch Capture & GPIO Trigger

| #   | İş Kalemi                                | Süre |
| --- | ---------------------------------------- | ---- |
| 3.7 | `CaptureService` batch capture API       | 2h   |
| 3.8 | `POST /capture/batch` endpoint           | 1h   |
| 3.9 | GPIO trigger (libgpiod) — pin konfig'den | 2h   |

### Sprint 13 — Piksel Kalibrasyonu & Mikroskopi Metadata

| #    | İş Kalemi                                 | Süre |
| ---- | ----------------------------------------- | ---- |
| 3.10 | µm/px profil alanı + DNG XMP'ye yazma     | 1.5h |
| 3.11 | Deney metadata endpoint (`POST /session`) | 1.5h |
| 3.12 | EXIF timezone fix (UTC+offset)            | 0.5h |
| 3.13 | Objektif bilgisi profil alanı             | 1.5h |

### Sprint 14 — LED Aydınlatma Kontrolü

| #    | İş Kalemi                                               | Süre |
| ---- | ------------------------------------------------------- | ---- |
| 3.14 | `IPeripheral` interface + `LedController` (I²C/PWM)     | 2h   |
| 3.15 | `GET /illumination?intensity=80&channel=white` endpoint | 1h   |
| 3.16 | Profil bazlı otomatik aydınlatma ayarı                  | 2h   |

---

## Faz 4 — Kalite & Sertifikasyon Hazırlığı (Sprint 15+ | süregelen)

| İş Kalemi                           | Öncelik |
| ----------------------------------- | ------- |
| OME-TIFF export (Z-stack desteği)   | 🟡      |
| Tile scan / mozaik capture          | 🟡      |
| DICOM dönüştürücü (temel WSI SOP)   | 🟢      |
| OpenAPI şeması üretimi              | 🟡      |
| Python SDK (`picam_client.py`)      | 🟡      |
| ISO 13485 tasarım dosyası hazırlama | 🟡      |

---

## Backlog & Notlar

Bu sprinte sığmayan ama takip edilmesi gereken kalemler:

- [ ] `packScanlineRaw10` NEON optimizasyonu
- [ ] UVC V4L2 expose yalıtımı (`uvc.c:445`)
- [ ] Multiplanar format ikinci plane desteği
- [ ] Kamera listeleme API (`GET /cameras`)
- [ ] NAS/SFTP otomatik transfer
- [ ] Disk doluluk izleme

---

## Hız Ölçümü

Her sprint sonunda şu soruları yanıtla ve bu tabloya ekle:

| Sprint | Planlanan (h) | Gerçekleşen (h) | Tamamlanan Kalemler | Notlar |
| ------ | ------------- | --------------- | ------------------- | ------ |
| S1     | 5             | —               | —                   | —      |
| S2     | 5             | —               | —                   | —      |

---

_Son güncelleme: 2026-03-04_
