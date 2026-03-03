# PiCam — UVC Gadget with HTTP Capture & Web UI

This project is an enhanced fork of the original **uvc-gadget** utility from the [libcamera project](https://gitlab.freedesktop.org/camera/uvc-gadget.git). It turns a Linux-based device (like a Raspberry Pi) into a standard USB Video Class (UVC) webcam with advanced features:

- **Concurrent RAW still capture** (DNG/JPEG) via HTTP — no interruption to the live UVC stream
- **Web-based control panel** with real-time camera adjustments
- **Named profile system** — save, edit, and recall camera presets (persisted on disk)
- **HTTPS with custom CA** — Chrome/Safari/Tor compatible SSL certificates

**Primary Development Platform:** Raspberry Pi Zero 2W

---

## Table of Contents

- [Key Features](#key-features)
- [Hardware Requirements](#hardware-requirements)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
  - [Step 1: System Update](#step-1-system-update)
  - [Step 2: Install Required Packages](#step-2-install-required-packages)
  - [Step 3: Build the Project](#step-3-build-the-project)
  - [Step 4: Network Configuration](#step-4-network-configuration)
  - [Step 5: Create Systemd Service](#step-5-create-systemd-service)
  - [Step 6: SSL / HTTPS Setup](#step-6-ssl--https-setup)
  - [Step 7: Nginx Reverse Proxy](#step-7-nginx-reverse-proxy)
- [Usage](#usage)
  - [Web UI](#web-ui)
  - [Manual Testing](#manual-testing)
  - [Capturing Still Images](#capturing-still-images)
- [API Reference](#api-reference)
- [Architecture](#architecture)
- [Troubleshooting](#troubleshooting)
- [Acknowledgments](#acknowledgments)

---

## Key Features

- **Standard UVC Webcam:** Provides standard MJPEG or uncompressed video streams over USB.

- **Concurrent RAW Still Capture:** Trigger a still image capture at any time without interrupting the UVC video stream. Operates asynchronously using a callback-based architecture.

- **DNG & JPEG Output:** Still images in DNG (full RAW with metadata) or JPEG format. DNG preserves unprocessed sensor data, color matrices, exposure time, and analogue gain for professional post-processing.

- **Web-Based Control Panel:** A modern, responsive UI served via HTTPS at `https://192.168.7.2/` for real-time camera control:
  - **Image controls** — Brightness, Contrast, Saturation, Sharpness (live preview)
  - **Exposure** — Auto/Manual toggle with microsecond precision
  - **Gain** — Auto or manual analogue gain
  - **Capture** — DNG or JPEG with configurable quality, save-as dialog
  - **UVC stream control** — Start/stop from the browser
  - **Connection status** — Real-time connectivity indicator

- **Named Profile System:** Save, edit, load, and delete camera presets by name. Profiles are persisted on-disk at `/home/picam/.picam_profiles.json` and survive reboots, page refreshes, and browser changes. Editing a profile with the same name updates it in-place (no duplicates).

- **Active Profile Deletion:** If the currently active profile is deleted, the camera and UI automatically reset to factory defaults.

- **HTTPS with SAN Certificates:** `setup-ssl.sh` generates a custom CA + server certificate with SAN entries (`IP.1`, `DNS.1-3`), `extendedKeyUsage = serverAuth`, and `subjectKeyIdentifier` — compatible with **Chrome**, **Safari**, **Firefox**, and **Tor Browser**.

- **USB NCM Network:** The Pi appears as a network device over USB (192.168.7.2), simplifying access without extra network infrastructure.

- **Libcamera Integration:** Uses the modern libcamera stack and its DNGWriter class for robust DNG creation.

---

## Hardware Requirements

- **Raspberry Pi Zero 2W** (or other Pi with OTG: Pi Zero, Pi 4)
- **Compatible Camera Module** (v2, v3, or HQ Camera)
- **microSD Card** (8GB+, Class 10)
- **USB Cable** with data support

---

## Prerequisites

- `libcamera` — Modern Linux camera stack
- `libjpeg` — JPEG compression
- `libtiff-4` — TIFF/DNG support
- `meson` / `ninja` — Build system
- `nginx` — HTTPS reverse proxy
- `openssl` — Certificate generation
- `dnsmasq` — DHCP server for USB network
- C++17 compiler, `git`

---

## Installation

### Step 1: System Update

1. Use **Raspberry Pi Imager** → **Raspberry Pi OS Lite** (64-bit)
2. Enable SSH, set username/password, configure WiFi (optional)
3. Boot, SSH in, and update:

```bash
sudo apt update
sudo apt full-upgrade -y
sudo reboot
```

### Step 2: Install Required Packages

```bash
# Enable USB OTG
echo "dtoverlay=dwc2,dr_mode=otg" | sudo tee -a /boot/firmware/config.txt

# Install dependencies
sudo apt install -y git meson ninja-build \
    libcamera-dev libjpeg-dev libtiff-dev \
    dnsmasq nginx openssl
```

### Step 3: Build the Project

```bash
git clone https://github.com/yasintuncerr/picam.git
cd picam

make configure
make picam
sudo make install
sudo ldconfig
```

After building, `picam` is at `/usr/bin/picam` and `picam-gadget.sh` at `/usr/bin/picam-gadget.sh`.

### Step 4: Network Configuration

Create the DHCP configuration for the USB interface:

```bash
sudo tee /etc/dnsmasq.d/usb-dhcp.conf <<EOF
interface=usb0
bind-interfaces
dhcp-range=192.168.7.10,192.168.7.50,24h
dhcp-option=3,192.168.7.2
dhcp-option=6,192.168.7.2
EOF
```

### Step 5: Create Systemd Service

```bash
sudo nano /etc/systemd/system/picam.service
```

```ini
[Unit]
Description=PiCam UVC Gadget with HTTP Still Capture
After=network.target local-fs.target
Before=dnsmasq.service

[Service]
Type=simple
User=root
WorkingDirectory=/root

ExecStartPre=/usr/bin/picam-gadget.sh start
ExecStartPre=/bin/sleep 2
ExecStartPre=/sbin/ip link set usb0 up
ExecStartPre=/sbin/ip addr add 192.168.7.2/24 dev usb0

ExecStart=/usr/bin/picam -c 0 -p 8080 uvc.0

ExecStopPost=/sbin/ip addr flush dev usb0
ExecStopPost=/sbin/ip link set usb0 down
ExecStopPost=/usr/bin/picam-gadget.sh stop

Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Configure dnsmasq dependency:

```bash
sudo mkdir -p /etc/systemd/system/dnsmasq.service.d
sudo tee /etc/systemd/system/dnsmasq.service.d/override.conf <<EOF
[Unit]
After=picam.service network-online.target
Wants=picam.service network-online.target
BindsTo=sys-subsystem-net-devices-usb0.device
After=sys-subsystem-net-devices-usb0.device
EOF
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable picam.service dnsmasq.service
sudo systemctl start picam.service
sleep 3
sudo systemctl start dnsmasq.service
```

### Step 6: SSL / HTTPS Setup

Generate certificates with proper SAN entries for Chrome/Safari/Tor compatibility:

```bash
cd ~/picam
sudo ./setup-ssl.sh
```

This creates:

- `/etc/nginx/ssl/picam-ca.crt` — CA certificate (copy to client machines)
- `/etc/nginx/ssl/picam-server.crt` — Server certificate with SAN
- `/etc/nginx/ssl/picam-server.key` — Server private key

The certificate includes:

- `subjectAltName` = `IP:192.168.7.2`, `DNS:picam`, `DNS:picam.local`, `DNS:picam.device`
- `extendedKeyUsage = serverAuth` — **required by Chrome**
- `subjectKeyIdentifier = hash`

**Trust the CA on Mac:**

```bash
scp picam@192.168.7.2:/etc/nginx/ssl/picam-ca.crt ~/Desktop/
sudo security add-trusted-cert -d -r trustRoot \
     -k /Library/Keychains/System.keychain ~/Desktop/picam-ca.crt
```

### Step 7: Nginx Reverse Proxy

Install the nginx config to serve the web UI and proxy API requests:

```bash
sudo cp ~/picam/nginx-picam.conf /etc/nginx/sites-available/picam
sudo ln -sf /etc/nginx/sites-available/picam /etc/nginx/sites-enabled/picam
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl restart nginx
sudo systemctl enable nginx
```

Nginx provides:

- **HTTPS** on port 443 with auto HTTP→HTTPS redirect
- Static file serving from `/home/picam/picam/www/`
- API proxy: `/capture`, `/video_controls`, `/reset_controls`, `/capture_controls`, `/profiles`, `/test_capture`, `/camera_status` → `localhost:8080`
- CA certificate download at `/picam-ca.crt`

---

## Usage

### Web UI

Open **`https://192.168.7.2/`** in your browser (Chrome, Safari, Firefox, or Tor).

**Ana Ekran (Main Screen):**

- Adjust Brightness / Contrast / Saturation / Sharpness with live preview
- Choose DNG or JPEG format, set JPEG quality
- Capture button with save-as dialog
- UVC stream start/stop control

**Ayarlar (Settings):**

- Exposure mode (Auto/Manual) with µs precision
- Gain control (Auto or manual)
- **Save Profile** — prompts for a name, saves to server (same name = edit/overwrite)
- **Reset to Defaults** — factory settings
- **Saved Profiles list** — click to load, ✕ to delete
  - Active profile highlighted with a left border
  - Deleting the active profile resets camera to defaults

Profiles persist across page refreshes, browser changes, and device reboots.

### Manual Testing

```bash
# Set up UVC gadget
sudo picam-gadget.sh start

# Start camera
picam -c 0 -p 8080 uvc.0
```

Options:

- `-c 0` — First libcamera device
- `-p 8080` — HTTP server port
- `uvc.0` — UVC function name

View the UVC stream with any webcam app (FaceTime, VLC, OBS, etc.).

### Capturing Still Images

```bash
# Default capture (uses saved profile)
curl -k https://192.168.7.2/capture -o capture.dng

# Auto capture via direct API
curl http://192.168.7.2:8080/capture > capture.dng

# Manual exposure (µs) + gain
curl "http://192.168.7.2:8080/capture?exposure=15000&gain=4.0" -o manual.dng

# JPEG capture
curl "http://192.168.7.2:8080/capture?format=jpeg&quality=95" -o capture.jpg

# Quick test capture (JPEG, uses saved profile)
curl http://192.168.7.2:8080/test_capture -o test.jpg
```

---

## API Reference

All endpoints accept `GET` requests. Parameters are passed as query strings.

### Capture

| Endpoint        | Description                                                           |
| --------------- | --------------------------------------------------------------------- |
| `/capture`      | Capture a still image (DNG or JPEG). Uses saved profile if no params. |
| `/test_capture` | Quick JPEG test capture using saved profile.                          |

**Parameters for `/capture`:**

| Parameter    | Type   | Default | Description                                                                                                     |
| ------------ | ------ | ------- | --------------------------------------------------------------------------------------------------------------- |
| `exposure`   | int64  | Auto    | Exposure time in µs                                                                                             |
| `gain`       | float  | Auto    | Analogue gain (>0 = manual)                                                                                     |
| `format`     | string | `dng`   | `dng` or `jpeg`                                                                                                 |
| `quality`    | int    | 90      | JPEG quality (1-100)                                                                                            |
| `brightness` | int    | 0       | -100 to +100                                                                                                    |
| `contrast`   | int    | 50      | 0-100 (50 = 1.0×)                                                                                               |
| `saturation` | int    | 50      | 0-100 (50 = 1.0×)                                                                                               |
| `sharpness`  | int    | 50      | 0-100 (50 = 1.0×)                                                                                               |
| `awb`        | string | `auto`  | AWB mode: `auto`, `daylight`, `cloudy`, `tungsten`, `fluorescent`, `indoor`, `incandescent`, `custom`, `manual` |

### Video Controls

| Endpoint              | Description                                                    |
| --------------------- | -------------------------------------------------------------- |
| `/video_controls?...` | Apply controls to the live viewfinder (same params as capture) |
| `/reset_controls`     | Reset video controls to defaults                               |

### Profile Management

| Endpoint                  | Description                                    |
| ------------------------- | ---------------------------------------------- |
| `/capture_controls`       | Get current active profile as JSON             |
| `/capture_controls?...`   | Update active profile (single unnamed profile) |
| `/capture_controls/reset` | Reset active profile to factory defaults       |

### Named Profiles

| Endpoint                    | Description                                                                  |
| --------------------------- | ---------------------------------------------------------------------------- |
| `/profiles`                 | List all named profiles + active profile name                                |
| `/profiles/save?name=X&...` | Save/update a named profile (same name = overwrite)                          |
| `/profiles/load?name=X`     | Load a named profile as active                                               |
| `/profiles/delete?name=X`   | Delete a named profile. Returns `"reset":true` if it was the active profile. |

**Example: `/profiles` response:**

```json
{
  "active": "Microscope 10x",
  "profiles": [
    {
      "name": "Microscope 10x",
      "exposure_us": 15000,
      "gain": 2.0,
      "awb_mode": "daylight",
      "brightness": 0,
      "contrast": 50,
      "saturation": 50,
      "sharpness": 60,
      "format": "dng",
      "jpeg_quality": 90
    }
  ]
}
```

### Camera Status

| Endpoint         | Description                                           |
| ---------------- | ----------------------------------------------------- |
| `/camera_status` | Real-time exposure_us and gain values from the sensor |

---

## Architecture

### Asynchronous Still Capture

1. HTTP request arrives at `/capture`
2. Client thread registers a callback with the libcamera still source
3. Client thread waits on a condition variable
4. libcamera processes the next frame for still capture
5. Callback copies buffer data and metadata
6. Client thread generates DNG/JPEG and serves as HTTP response

Video stream continues uninterrupted throughout.

### Profile Persistence

Named profiles are stored as a JSON array in `/home/picam/.picam_profiles.json`. The active capture profile is also saved to `/home/picam/.picam_capture_profile.json`. Both files are loaded on service startup and written on every profile change.

**Max profiles:** 20 (embedded device constraint).

### Network Architecture

```
  [Host Computer]
       │
       │ USB (UVC video + NCM network)
       │
  [Raspberry Pi Zero 2W]
       │
       ├── picam (port 8080) ── UVC stream + HTTP API
       │
       ├── nginx (port 443) ── HTTPS reverse proxy
       │         └── Static files: /home/picam/picam/www/
       │
       └── dnsmasq ── DHCP for USB network (192.168.7.x)
```

### DNG Writer

Derived from libcamera's DNGWriter class. Handles RAW buffer formatting, TIFF/EXIF metadata, color matrix calculations, and proper byte ordering.

---

## Troubleshooting

### Camera not detected

- Check ribbon cable connection
- Enable camera: `sudo raspi-config` → Interface Options → Camera
- Verify: `libcamera-hello --list-cameras`

### UVC device not appearing on host

- Use a data-capable USB cable (not power-only)
- Check OTG overlay: `dtoverlay -l | grep dwc2`
- Check modules: `lsmod | grep libcomposite`

### SSL certificate rejected by Chrome

- Ensure `setup-ssl.sh` was run (check `extendedKeyUsage`):
  ```bash
  openssl x509 -in /etc/nginx/ssl/picam-server.crt -noout -text | grep -A2 "Extended Key Usage"
  ```
- Re-trust the CA on your Mac after regenerating certificates
- Clear browser cache / restart browser

### Network interface not working

- Verify dnsmasq: `systemctl status dnsmasq`
- Check interface: `ip addr show usb0`
- On host, configure IP to `192.168.7.1/24` if DHCP doesn't work

### Profiles not persisting

- Check file permissions: `ls -la /home/picam/.picam_profiles.json`
- Check service logs: `sudo journalctl -u picam.service -n 30`
- Ensure the picam process has write access to `/home/picam/`

### Service fails to start

- Check logs: `sudo journalctl -u picam.service -n 50`
- Verify camera not in use: `fuser /dev/video*`
- Check dependencies: `systemctl list-dependencies picam.service`

### View live logs

```bash
sudo journalctl -u picam.service -f
sudo journalctl -u nginx.service -f
sudo journalctl -u dnsmasq.service -f
```

---

## Acknowledgments

- **[uvc-gadget](https://gitlab.freedesktop.org/camera/uvc-gadget.git)** — Original UVC gadget implementation from the libcamera project.
- **[libcamera](https://libcamera.org/)** — Modern Linux camera stack. DNGWriter derived from libcamera source.
- **Raspberry Pi Foundation** — Hardware and software ecosystem.

---

## Contributing

Contributions are welcome! Please submit issues, feature requests, or pull requests.

---

## License

Licensed under **LGPL-2.1-or-later**, consistent with upstream uvc-gadget and libcamera.

---

**Repository:** [https://github.com/yasintuncerr/picam](https://github.com/yasintuncerr/picam)
