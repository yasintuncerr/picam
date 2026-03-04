# PiCam — Hedef Mimari

> Bu belge mevcut kodun mimari sorunlarını tespit eder ve hedef clean architecture'ı tanımlar.  
> Refactor sürecinde tüm kararlar bu belgeden referans alır.

---

## 1. Mevcut Mimari ve Sorunları

### 1.1 Katman Haritası (As-Is)

```
┌─────────────────────────────────────────────────────────────┐
│                     main.c                                   │
│  (arg parsing, UVC setup, event loop başlatma)               │
└───────────────┬─────────────────────────────────────────────┘
                │
        ┌───────▼────────┐         ┌──────────────────┐
        │   capture.c    │         │  uvc.c / v4l2.c  │
        │  (HTTP server, │         │  (UVC gadget, V4L2│
        │  profile CRUD, │         │  device control) │
        │  JSON hacks)   │         └──────────────────┘
        └───────┬────────┘
                │ extern C calls (C↔C++ boundary yok)
        ┌───────▼──────────────────────────────────────┐
        │            libcamera-source.cpp               │
        │  (CameraManager, stream config, MJPEG encode, │
        │   DNG write, event pipe, still capture)       │
        │  ← TEK DOSYADA HER ŞEY                        │
        └───────┬──────────────────────────────────────┘
                │
        ┌───────▼──────────┐
        │  dng-writer.cpp  │
        │  (TIFF/DNG write │
        │   buffer-to-file)│
        └──────────────────┘
```

### 1.2 Tespit Edilen Mimari Sorunlar

| #   | Sorun                                                                                                 | Etkisi                                                           |
| --- | ----------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------- |
| 1   | **God object**: `libcamera-source.cpp` hem camera mgmt, hem encode, hem DNG write, hem event handling | Değiştirilemez, test edilemez                                    |
| 2   | **C/C++ karışık boundary**: extern C ile C++ state paylaşılıyor                                       | Type safety yok, ABI kırılgan                                    |
| 3   | **pthread-per-client HTTP**: ölçeklenmiyor, socket sızıntısı riski                                    | Yük altında çöker                                                |
| 4   | **Senkron capture bekleme**: client thread HTTP timeout'a kadar bloklu                                | Eşzamanlı istek gelince deadlock                                 |
| 5   | **Hardcoded her şey**: çözünürlük, port, profil yolu, JPEG kalitesi                                   | Yeni sensör/ortam = derleme gerekir                              |
| 6   | **Global state**: `controls_dirty_`, `restore_pending_` struct içinde dağınık                         | Yarış koşulu, debug edilemez                                     |
| 7   | **fscanf ile JSON parse**: format değişince sessizce çöker                                            | Veri kaybı                                                       |
| 8   | **Event loop: pipe()**: libcamera callback → pipe write → fd read döngüsü                             | Gereksiz syscall, yüksek FPS'de gecikme                          |
| 9   | **Sıfır test altyapısı**: hiç unit/integration test yok                                               | Refactor riski çok yüksek                                        |
| 10  | **Logging: stderr/printf karışımı**: seviyesiz, filtrelenemiyor                                       | Prodüksiyonda debug imkânsız                                     |
| 11  | **Stream = UVC hardcoded**: tek çıkış yolu UVC gadget; UDP/RTSP/H.264/H.265 eklenemiyor               | Ağ üzerinden stream, kayıt, laboratuvar entegrasyonu yapılamıyor |

---

## 2. Hedef Mimari (To-Be)

### 2.1 Katman Diyagramı

```
┌──────────────────────────────────────────────────────────────────┐
│                         Presentation Layer                        │
│  ┌────────────────┐  ┌─────────────────┐  ┌──────────────────┐  │
│  │  HTTP/REST API │  │  WebSocket/SSE  │  │  CLI (picam-cli) │  │
│  │  (mongoose)    │  │  (status push)  │  │  (automation)    │  │
│  └───────┬────────┘  └─────────────────┘  └──────────────────┘  │
└──────────┼───────────────────────────────────────────────────────┘
           │
┌──────────┼───────────────────────────────────────────────────────┐
│          │              Application Layer                          │
│  ┌───────▼──────────┐  ┌──────────────────┐  ┌───────────────┐  │
│  │  CaptureService  │  │  ProfileService  │  │  StageService │  │
│  │  (orchestrate)   │  │  (CRUD + persist)│  │  (XY, focus)  │  │
│  └───────┬──────────┘  └──────────────────┘  └───────┬───────┘  │
│          │              StreamManager                  │          │
│  ┌───────▼──────────────────────────────────────────┐ │          │
│  │  IStreamSink (pluggable — mod seçimi runtime)    │ │          │
│  │  ┌──────────┐ ┌──────────┐ ┌────────┐ ┌───────┐ │ │          │
│  │  │UvcSink   │ │RtspSink  │ │UdpSink │ │HlsSink│ │ │          │
│  │  │(mevcut)  │ │H.264/265 │ │raw/rtp │ │(HLS)  │ │ │          │
│  │  └──────────┘ └──────────┘ └────────┘ └───────┘ │ │          │
│  └──────────────────────────────────────────────────┘ │          │
└──────────┼────────────────────────────────────────────┼──────────┘
           │ abstract interfaces                         │
┌──────────┼────────────────────────────────────────────┼──────────┐
│          │              Domain / Core Layer            │          │
│  ┌───────▼──────────┐  ┌──────────────────┐  ┌───────▼───────┐  │
│  │  CameraPipeline  │  │   ImageProcessor │  │  MetadataStore│  │
│  │  (frame dispatch │  │  (flatfield,     │  │  (DNG/OME     │  │
│  │   → sink router) │  │   focus quality) │  │   metadata)   │  │
│  └───────┬──────────┘  └──────────────────┘  └───────────────┘  │
└──────────┼───────────────────────────────────────────────────────┘
           │
┌──────────┼───────────────────────────────────────────────────────┐
│          │              Infrastructure Layer                        │
│  ┌───────▼──────────┐  ┌──────────────────┐  ┌───────────────┐  │
│  │  LibcameraBackend│  │   DngWriter      │  │PeripheralBus  │  │
│  │  (libcamera API) │  │  (libtiff)       │  │ I²C/GPIO/UART │  │
│  └──────────────────┘  └──────────────────┘  └───────────────┘  │
│  ┌───────────────────┐  ┌────────────────────────────────────┐   │
│  │  StorageBackend   │  │  Logger / Config / EventLoop       │   │
│  │  (local/NAS/SFTP) │  │  (spdlog / toml++ / epoll)        │   │
│  └───────────────────┘  └────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 Modül Sınırları ve Sorumluluklar

#### `core/camera_pipeline` — Kamera Akışı Yönetimi

- libcamera `CameraManager`, `CameraConfiguration`, `Request` yaşam döngüsü
- Video stream ↔ still stream ayrımı
- `ICameraBackend` arayüzü üzerinden test mock'u mümkün
- **Bağımlılık**: yalnızca libcamera + core/event

#### `core/image_processor` — Görüntü İşleme

- Flatfield / darkfield düzeltme
- Histogram hesaplama
- Odak kalitesi metriği (Laplacian varyansı)
- **Bağımlılık**: yok (pure C++ veya OpenCV optional)

#### `core/metadata` — DNG / OME-TIFF Metadata

- DNG tag yönetimi (`BaselineExposure`, `ForwardMatrix1` vb.)
- Mikroskopi metadata (objektif, µm/px, deney ID)
- OME-XML üretimi
- **Bağımlılık**: libtiff

#### `services/capture_service` — Capture Orkestratörü

- Batch capture, sekans yönetimi
- Capture queue (ring buffer)
- GPIO trigger dinleme
- **Bağımlılık**: camera_pipeline + image_processor + storage

#### `services/profile_service` — Profil Yönetimi

- Named profile CRUD
- JSON serializasyon (nlohmann/json ile)
- Aktif profil state'i
- **Bağımlılık**: yok (pure data)

#### `services/stream_manager` — Stream Yöneticisi

- Aktif sink'leri yönetir; aynı anda birden fazla sink aktif olabilir
- `picam.toml`'dan sink listesi ve parametreler okunur
- Runtime sink ekleme/çıkarma (`POST /stream/start`, `DELETE /stream/stop`)
- Her frame `CameraPipeline`'dan gelir → aktif sink'lere fan-out ile dağıtılır
- **Bağımlılık**: core/camera + infra/config

#### `api/http_server` — HTTP API

- Tek event-loop tabanlı server (mongoose)
- Route handler'lar → service katmanına delegate
- OpenAPI şeması üretimi
- **Bağımlılık**: services/\*

#### `infra/config` — Yapılandırma

- `picam.toml` okuma (toml++)
- Ortam değişkeni override
- Runtime yeniden yükleme (SIGHUP)

#### `infra/logger` — Loglama

- spdlog entegrasyonu
- Seviyeli log (TRACE/DEBUG/INFO/WARN/ERROR)
- JSON formatter + console formatter

### 2.3 Dizin Yapısı (To-Be)

```
picam/
├── src/
│   └── main.cpp                  ← sadece DI wiring + start
├── core/
│   ├── camera/
│   │   ├── ICameraBackend.h      ← pure interface
│   │   ├── CameraPipeline.h/cpp  ← frame dispatch → sink router
│   │   └── LibcameraBackend.h/cpp
│   ├── image/
│   │   ├── ImageProcessor.h/cpp  ← flatfield, focus, histogram
│   │   └── FocusMetric.h/cpp
│   └── metadata/
│       ├── DngMetadata.h/cpp
│       └── OmeTiffWriter.h/cpp
├── services/
│   ├── CaptureService.h/cpp      ← batch, queue, trigger
│   ├── ProfileService.h/cpp
│   ├── StageService.h/cpp
│   └── StreamManager.h/cpp       ← sink fan-out yöneticisi
├── stream/
│   ├── IStreamSink.h             ← pure interface (tüm sinkler implement eder)
│   ├── UvcSink.h/cpp             ← mevcut UVC gadget (izole)
│   ├── RtspSink.h/cpp            ← H.264/H.265 → RTSP (live555 / mediamtx)
│   ├── UdpSink.h/cpp             ← raw RTP/UDP (düşük gecikme, LAN)
│   ├── HlsSink.h/cpp             ← HLS segmentleri (web tarayıcı erişimi)
│   └── RecordSink.h/cpp          ← MP4/MKV kayıt (ffmpeg pipe)
├── api/
│   ├── HttpServer.h/cpp
│   ├── routes/
│   │   ├── CaptureRoutes.h/cpp
│   │   ├── StreamRoutes.h/cpp    ← /stream/* endpoint'leri
│   │   ├── ProfileRoutes.h/cpp
│   │   └── StatusRoutes.h/cpp
│   └── WebSocketServer.h/cpp
├── infra/
│   ├── Config.h/cpp
│   ├── Logger.h/cpp
│   ├── EventLoop.h/cpp
│   └── peripherals/
│       ├── IPeripheral.h
│       ├── GpioTrigger.h/cpp
│       └── LedController.h/cpp
├── uvcgadget/                    ← mevcut UVC kodu (UvcSink tarafından sarmalanır)
├── docs/
├── tests/
│   ├── unit/
│   │   ├── test_profile_service.cpp
│   │   ├── test_focus_metric.cpp
│   │   ├── test_dng_metadata.cpp
│   │   └── test_stream_manager.cpp
│   └── integration/
│       └── smoke_test.sh
└── meson.build
```

### 2.4 Bağımlılık Kuralları (Dependency Rule)

```
Presentation → Application → Domain ← Infrastructure
                    ↑
              (interfaces only)
```

- **Katmanlar yalnızca bir alt katmanı import edebilir**
- `core/` hiçbir harici framework'e bağımlı olamaz (test edilebilirlik)
- `infra/` libcamera, libtiff, spdlog vs. içerebilir
- Her modül kendi mock'unu sağlar (`MockCameraBackend`)

### 2.5 Anahtar Interface'ler

```cpp
// core/camera/ICameraBackend.h
class ICameraBackend {
public:
    virtual ~ICameraBackend() = default;
    virtual int  start(const CameraConfig &cfg) = 0;
    virtual void stop() = 0;
    virtual int  captureStill(const CaptureRequest &req,
                              StillCallback cb) = 0;
    virtual CameraProperties properties() const = 0;
};

// stream/IStreamSink.h
struct VideoFrame {
    const uint8_t *data;
    size_t         size;
    int64_t        timestamp_us;
    uint32_t       width, height;
    PixelFormat    format;       // MJPEG | YUV420 | H264 | H265
};

class IStreamSink {
public:
    virtual ~IStreamSink() = default;
    virtual int  start(const SinkConfig &cfg) = 0;
    virtual void stop() = 0;
    virtual void onFrame(const VideoFrame &frame) = 0;  // non-blocking!
    virtual std::string type() const = 0;  // "uvc", "rtsp", "udp", "hls"
    virtual SinkStatus  status() const = 0;
};

// services/StreamManager.h
class StreamManager {
public:
    // Konfig'den sink'leri yükle
    void loadFromConfig(const Config &cfg);
    // Runtime ekleme/çıkarma
    int  addSink(std::unique_ptr<IStreamSink> sink);
    void removeSink(std::string_view type);
    // CameraPipeline bu callback'i çağırır
    void onFrame(const VideoFrame &frame);  // → tüm aktif sink'lere fan-out
    std::vector<SinkStatus> status() const;
};

// services/CaptureService.h
class CaptureService {
public:
    using DoneCallback = std::function<void(CaptureResult)>;
    void capture(CaptureRequest req, DoneCallback cb);
    void captureBatch(BatchRequest req, DoneCallback cb);
    void setTrigger(std::unique_ptr<ITrigger> trigger);
};

// infra/Logger.h
namespace picam::log {
void info(std::string_view module, std::string_view msg);
void error(std::string_view module, std::string_view msg);
}
```

---

## 3. Teknoloji Seçimleri

| Bileşen         | Mevcut                   | Hedef                                                | Neden                                    |
| --------------- | ------------------------ | ---------------------------------------------------- | ---------------------------------------- |
| HTTP server     | raw BSD socket + pthread | **mongoose** (single-file, async)                    | Event-driven, embeddable, TLS built-in   |
| JSON            | `fscanf` / `sprintf`     | **nlohmann/json** (header-only)                      | Type-safe, standart                      |
| Config          | yok                      | **toml++** (header-only)                             | İnsan okunabilir, tip güvenli            |
| Logging         | `fprintf(stderr)`        | **spdlog** (header-only)                             | Çok hızlı, seviyeli, async               |
| Event loop      | `pipe()` + select        | **epoll** (doğrudan)                                 | Syscall azalt, Pi Zero'da düşük overhead |
| Test            | yok                      | **Catch2** (header-only)                             | Minimal setup, BDD syntax                |
| Build           | meson                    | meson (koru)                                         | Mevcut + cross-compile desteği           |
| H.264/H.265 enc | yok                      | **libav (FFmpeg)** veya **V4L2 M2M HW encoder**      | Pi Zero 2W V4L2 H.264 HW encoder var     |
| RTSP server     | yok                      | **live555** (embeddable) veya **mediamtx** (sidecar) | Mikroskop yazılımları RTSP tüketir       |
| RTP/UDP         | yok                      | raw socket (minimal latency, LAN için)               | <5ms gecikme, kablo bağlantı             |

---

## 4. Stream Mimarisi Detayı

### 4.1 Sink Karşılaştırması

| Sink         | Protokol          | Codec         | Gecikme  | Kullanım Senaryosu                        |
| ------------ | ----------------- | ------------- | -------- | ----------------------------------------- |
| `UvcSink`    | USB gadget        | MJPEG / YUV   | <16ms    | Host'a USB webcam olarak görünme          |
| `RtspSink`   | RTSP/TCP veya UDP | H.264 / H.265 | 50–200ms | Mikroskop yazılımı, VLC, OBS, kayıt       |
| `UdpSink`    | RTP/UDP           | MJPEG / H.264 | <10ms    | LAN içi düşük gecikme, laboratuvar ekranı |
| `HlsSink`    | HTTP (HLS)        | H.264         | 2–6s     | Tarayıcıdan ek yazılım olmadan izleme     |
| `RecordSink` | Dosya             | H.264 / H.265 | —        | Deneysel kayıt, zaman serisi analizi      |

### 4.2 Mikroskop için Önerilen Kombinasyon

```toml
# picam.toml
[stream]
default_sinks = ["uvc", "rtsp"]

[stream.uvc]
enabled = true
format  = "mjpeg"        # USB webcam uyumu

[stream.rtsp]
enabled  = true
port     = 8554
path     = "/microscope"
codec    = "h264"        # Pi Zero 2W V4L2 HW encoder
bitrate  = 8000000       # 8 Mbps — histopatoloji için yeterli
fps      = 15            # still sırasında düşürülebilir

[stream.udp]
enabled = false
host    = "192.168.7.1"
port    = 5004
```

### 4.3 Frame Dağıtım Akışı

```
libcamera frame callback
        │
        ▼
  CameraPipeline
   (decode / map)
        │
        ├──→ ImageProcessor (flatfield düzeltme, in-place)
        │
        ▼
  StreamManager::onFrame()
        │
        ├──→ UvcSink::onFrame()     [MJPEG → USB]
        ├──→ RtspSink::onFrame()    [H.264 HW enc → RTSP]
        ├──→ UdpSink::onFrame()     [RTP → UDP]
        └──→ RecordSink::onFrame()  [H.264 → MP4 dosyası]

  (Tüm sink'ler non-blocking; geç sink'ler frame'i drop eder)
```

### 4.4 H.264 Codec Seçimi: Pi Zero 2W'de HW Encode

Pi Zero 2W'deki VideoCore VI, **V4L2 M2M H.264 encoder** sunar:

```bash
# Doğrulama
v4l2-ctl --list-devices | grep -A2 "bcm2835"
# /dev/video11 → H.264 encoder
# /dev/video12 → H.264 decoder
```

- libcamera'dan YUV420 al → `/dev/video11` → H.264 bitstream → RtspSink / UdpSink
- FFmpeg `libav` ise SW fallback (Pi Zero'da yavaş, ancak H.265 için gerekebilir)
- **H.265**: Pi Zero 2W'de HW H.265 **yok**; FFmpeg SW encode çok yavaş → gerekiyorsa harici Pi 4/5 relay

### 4.5 Stream API Endpoint'leri

```
GET  /stream/status          → aktif sink'ler, codec, fps, bitrate
POST /stream/start?type=rtsp → runtime RTSP sink ekle
DEL  /stream/stop?type=rtsp  → runtime RTSP sink kaldır
GET  /stream/rtsp/url        → rtsp://192.168.7.2:8554/microscope
PUT  /stream/config          → bitrate, fps, codec güncelle (yeniden başlatır)
```

---

_Bu belge refactor boyunca güncellenmelidir. Mimari kararlar buraya not düşülür._
