# UVC Gadget with HTTP Still Capture

This project is an enhanced fork of the original **uvc-gadget** utility from the [libcamera project](https://gitlab.freedesktop.org/camera/uvc-gadget.git). The original application provides a robust way to turn a Linux-based device (like a Raspberry Pi) into a standard USB Video Class (UVC) webcam.

This fork extends the base functionality by adding a critical new feature: the ability to capture high-resolution, full-metadata RAW still images in DNG format via an HTTP endpoint, **concurrently** with the active UVC video stream.

**Primary Development Platform:** Raspberry Pi Zero 2W

---

## Table of Contents

- [Key Features](#key-features)
- [Purpose and Motivation](#purpose-and-motivation)
- [Hardware Requirements](#hardware-requirements)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
  - [Step 1: System Update](#step-1-system-update)
  - [Step 2: Install Required Packages](#step-2-install-required-packages)
  - [Step 3: Build the Project](#step-3-build-the-project)
  - [Step 4: Network Configuration](#step-4-network-configuration)
  - [Step 5: Create Systemd Service](#step-5-create-systemd-service)
- [Usage](#usage)
  - [Manual Testing](#manual-testing)
  - [Capturing Still Images](#capturing-still-images)
- [Architecture](#architecture)
- [Acknowledgments](#acknowledgments)
- [License](#license)

---

## Key Features

* **Standard UVC Webcam Functionality:** Inherits all features from the upstream `uvc-gadget`, providing standard MJPEG or uncompressed video streams over USB.

* **Concurrent RAW Still Capture:** The primary enhancement. You can trigger a still image capture at any time without interrupting or stopping the UVC video stream.

* **Asynchronous Operation:** The still capture system now operates asynchronously using a callback-based architecture. When a capture is requested via HTTP, the system registers a callback and continues serving the video stream. Once the camera hardware completes the capture, the callback is invoked, and the DNG file is generated and served without blocking the main video pipeline.

* **DNG Format Output:** Still images are saved in the Digital Negative (DNG) format. This preserves the unprocessed RAW data from the camera sensor, along with rich metadata (exposure time, analog gain, color matrices, etc.), making it ideal for professional post-processing and computer vision tasks. The DNG writer implementation is derived from libcamera's DNGWriter class.

* **HTTP Server Endpoint:** A lightweight HTTP server is integrated into the application. A simple `GET` request to the `/capture` endpoint triggers the still capture and serves the resulting DNG file.

* **USB NCM Network Support:** Includes configuration for USB Network Control Model (NCM), allowing the Pi to appear as a network device over USB, simplifying HTTP access without requiring additional network infrastructure.

* **Libcamera Integration:** Leverages the modern `libcamera` stack for camera control and utilizes its `DNGWriter` class for robust DNG file creation.

---

## Purpose and Motivation

The goal of this project is to create a versatile camera solution for advanced applications where both a live video feed and high-quality, unprocessed still images are required. This is particularly useful for:

---

## Hardware Requirements

* **Raspberry Pi Zero 2W** (primary development platform) or other Raspberry Pi models with OTG support (Pi Zero, Pi 4, etc.)
* **Compatible Camera Module** (Raspberry Pi Camera Module v2, v3, or HQ Camera)
* **microSD Card** (8GB or larger, Class 10 recommended)
* **USB Cable** with data support for connecting to host computer

---

## Prerequisites

The project requires the following libraries and tools:

* `libcamera` - Modern Linux camera stack
* `libjpeg` - JPEG image compression
* `libtiff-4` - TIFF image format support
* `meson` - Build system
* `ninja` - Build tool
* A C++17 compliant compiler
* `git` - Version control
* `dnsmasq` - DHCP server (for network configuration)

---

## Installation

This guide provides step-by-step instructions for setting up the UVC gadget with HTTP still capture on a Raspberry Pi Zero 2W running Raspberry Pi OS Lite.

### Step 1: System Update

First, prepare your Raspberry Pi SD card:

1. Use **Raspberry Pi Imager** to write Raspberry Pi OS to your SD card
2. Choose **Raspberry Pi OS Lite** (64-bit recommended for Pi Zero 2W)
3. Configure settings before writing:
   - Enable SSH
   - Set username and password
   - Configure WiFi (optional, for initial setup)
4. Insert the SD card into your Pi and power it on
5. Connect via SSH and update the system:

```bash
sudo apt update
sudo apt full-upgrade -y
sudo reboot
```

### Step 2: Install Required Packages

After the reboot, reconnect via SSH and install all necessary packages:

```bash
# Enable USB OTG functionality
echo "dtoverlay=dwc2,dr_mode=otg" | sudo tee -a /boot/firmware/config.txt

# Install build dependencies
sudo apt install -y git meson ninja-build \
    libcamera-dev libjpeg-dev libtiff-dev \
    dnsmasq
```

### Step 3: Build the Project

Clone and build the picam application:

```bash
# Clone the repository
git clone https://github.com/yasintuncerr/picam.git
cd picam

# Configure the build
make configure

# Build the project
make picam

# Install system-wide
sudo make install

# Update library cache
sudo ldconfig
```

After installation, the `picam` executable will be available at `/usr/bin/picam`, and the `picam-gadget.sh` script will be at `/usr/bin/picam-gadget.sh`.

### Step 4: Network Configuration

To enable HTTP capture functionality over USB, configure the Pi to act as a USB network device:

#### Configure USB Network Interface

Edit the DHCP configuration:

```bash
sudo nano /etc/dhcpcd.conf
```

Add the following lines at the end of the file:

```
# USB OTG Network Interface
interface usb0
static ip_address=192.168.7.2/24
```

Save and exit (Ctrl+X, Y, Enter).

#### Configure DHCP Server

Create a DHCP configuration for the USB interface:

```bash
sudo nano /etc/dnsmasq.d/usb-dhcp.conf
```

Add the following configuration:

```
interface=usb0
bind-interfaces
dhcp-range=192.168.7.10,192.168.7.50,24h
dhcp-option=3,192.168.7.2
dhcp-option=6,192.168.7.2
```

Save and exit.

#### Restart Network Services

```bash
sudo systemctl restart dhcpcd
sudo systemctl restart dnsmasq
sudo systemctl enable dnsmasq
```

Verify dnsmasq is running:

```bash
systemctl status dnsmasq
```

### Step 5: Create Systemd Service

To have the UVC gadget start automatically on boot, create a systemd service:

```bash
sudo nano /etc/systemd/system/picam.service
```

Add the following service configuration:

```ini
[Unit]
Description=PiCam UVC Gadget with HTTP Still Capture
After=network.target local-fs.target

[Service]
Type=simple
User=root
WorkingDirectory=/root

# First setup the UVC gadget
ExecStartPre=/usr/bin/picam-gadget.sh start

# Start the camera application
ExecStart=/usr/bin/picam -c 0 -p 8080 uvc.0

# Cleanup on stop
ExecStopPost=/usr/bin/picam-gadget.sh stop

# Restart on failure
Restart=on-failure
RestartSec=5

# Logging
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Save and exit.

#### Enable and Start the Service

```bash
# Reload systemd to recognize the new service
sudo systemctl daemon-reload

# Enable the service to start on boot
sudo systemctl enable picam.service

# Start the service immediately
sudo systemctl start picam.service

# Check the service status
sudo systemctl status picam.service
```

#### View Service Logs

To monitor the service output:

```bash
# View live logs
sudo journalctl -u picam.service -f

# View recent logs
sudo journalctl -u picam.service -n 50
```

---

## Usage

### Manual Testing

For testing purposes, you can run the application manually:

1. **Connect the Raspberry Pi to your host computer** via USB (use the USB port, not the power port)

2. **Set up the UVC gadget:**
   ```bash
   sudo picam-gadget.sh start
   ```

3. **Start the camera application:**
   ```bash
   picam -c 0 -p 8080 uvc.0
   ```
   
   Options:
   - `-c 0`: Use the first libcamera device (camera index 0)
   - `-p 8080`: Start HTTP server on port 8080
   - `uvc.0`: UVC function name

4. **View the video stream:** The device will enumerate as a standard UVC webcam on your host computer. You can view the stream using:
   - **Windows:** Camera app, VLC, OBS Studio
   - **macOS:** FaceTime, QuickTime Player, VLC
   - **Linux:** VLC, guvcview, ffmpeg

5. **Configure the USB network interface on your host:**
   
   When you first connect, your host computer should detect a new network interface. Configure it manually:
   
   - **IP Address:** `192.168.7.1` (or any IP in the 192.168.7.x range except 192.168.7.2)
   - **Subnet Mask:** `255.255.255.0`
   - **Gateway:** `192.168.7.2`
   
   Or enable DHCP to automatically receive an IP from the Pi.

### Capturing Still Images

Once the camera is running and the network is configured, you can capture RAW DNG images via HTTP:

```bash
# Capture and save a DNG file
curl http://192.168.7.2:8080/capture > my_capture.dng

# Or use wget
wget http://192.168.7.2:8080/capture -O my_capture.dng
```

The capture happens asynchronously:
1. The HTTP request triggers a capture request
2. The camera completes the capture in the background
3. The callback is invoked with the captured buffer
4. The DNG file is generated on-the-fly
5. The file is served as the HTTP response

This process does not interrupt or affect the ongoing UVC video stream.

**Note:** Each capture request may take a few seconds depending on camera settings and resolution. The HTTP connection will remain open until the DNG file is fully generated and transmitted.

---

## Architecture

### Asynchronous Still Capture Design

The still capture system has been redesigned with an asynchronous, callback-based architecture:

**Flow:**
1. HTTP request arrives at `/capture` endpoint
2. Client thread registers a callback with the libcamera still source
3. Client thread waits on a condition variable
4. Libcamera processes the next available frame for still capture
5. When ready, libcamera invokes the callback with the raw buffer
6. Callback copies buffer data and metadata
7. Client thread wakes up and generates the DNG file using the captured data
8. DNG file is served as HTTP response

**Benefits:**
- Non-blocking: Video stream continues uninterrupted during still capture
- Efficient: No polling or busy-waiting
- Scalable: Multiple capture requests can be queued
- Clean separation: Camera event loop remains independent

### DNG Writer Implementation

The DNG writer is derived from libcamera's DNGWriter class with modifications for integration with the HTTP server. It handles:
- RAW buffer data formatting
- TIFF/EXIF metadata generation
- Color matrix calculations
- Proper byte ordering and data packing

Original source: [libcamera DNGWriter](https://git.libcamera.org/libcamera/libcamera.git)


---

## Acknowledgments

This work would not be possible without the excellent foundational projects and code:

* **[uvc-gadget](https://gitlab.freedesktop.org/camera/uvc-gadget.git)** - The original UVC gadget implementation from the libcamera project, providing the core UVC functionality.

* **[libcamera](https://libcamera.org/)** - The modern Linux camera stack that powers this application. The DNGWriter implementation is derived from libcamera's source code.

* **Raspberry Pi Foundation** - For the excellent hardware and software ecosystem that makes projects like this possible.

Special thanks to all contributors to these projects for their dedication to open-source camera support on Linux.

---

## Contributing

Contributions are welcome! Please feel free to submit issues, feature requests, or pull requests.

---

## License

This project is licensed under the **LGPL-2.1-or-later**, consistent with the upstream `uvc-gadget` project and libcamera.


---

## Troubleshooting

### Camera not detected
- Ensure the camera ribbon cable is properly connected
- Check if the camera is enabled: `sudo raspi-config` → Interface Options → Camera
- Verify camera detection: `libcamera-hello --list-cameras`

### UVC device not appearing on host
- Check USB cable supports data transfer (not power-only)
- Verify OTG overlay is loaded: `dtoverlay -l | grep dwc2`
- Check kernel modules: `lsmod | grep libcomposite`

### Network interface not working
- Verify dnsmasq is running: `systemctl status dnsmasq`
- Check USB network interface exists: `ip addr show usb0`
- On host, check for new network interface and configure manually if DHCP doesn't work

### Service fails to start
- Check service logs: `sudo journalctl -u picam.service -n 50`
- Verify permissions: The service runs as root to access USB gadget
- Ensure camera is not in use by another process

---

## Future Work

- **Optimized DNG Writing:** Move DNG file generation to a separate worker thread pool to further minimize any potential latency
- **Configurable Capture Settings:** Allow HTTP parameters to control exposure, gain, and resolution for still captures
- **Multiple Format Support:** Add support for JPEG and TIFF output in addition to DNG
- **Authentication:** Add optional HTTP authentication for capture endpoint
- **WebSocket Support:** Real-time capture notifications and status updates

---

**For more information, visit:** [https://github.com/yasintuncerr/picam](https://github.com/yasintuncerr/picam)
