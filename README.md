# UVC Gadget with HTTP Still Capture

This project is an enhanced fork of the original **uvc-gadget** utility from the [libcamera project](https://gitlab.freedesktop.org/camera/uvc-gadget). The original application provides a robust way to turn a Linux-based device (like a Raspberry Pi) into a standard USB Video Class (UVC) webcam.

This fork extends the base functionality by adding a critical new feature: the ability to capture high-resolution, full-metadata RAW still images in DNG format via an HTTP endpoint, **concurrently** with the active UVC video stream.

- - -

## Key Features

* **Standard UVC Webcam Functionality:** Inherits all features from the upstream `uvc-gadget`, providing a standard MJPEG or uncompressed video stream over USB.
* **Concurrent RAW Still Capture:** The primary enhancement. You can trigger a still image capture at any time without interrupting or stopping the UVC video stream.
* **DNG Format Output:** Still images are saved in the Digital Negative (DNG) format. This preserves the unprocessed RAW data from the camera sensor, along with rich metadata (exposure time, analog gain, color matrices, etc.), making it ideal for professional post-processing and computer vision tasks.
* **HTTP Server Endpoint:** A lightweight HTTP server is integrated into the application. A simple `GET` request to the `/capture` endpoint triggers the still capture and serves the resulting DNG file.
* **Libcamera Integration:** Leverages the modern `libcamera` stack for camera control and utilizes its `DNGWriter` class for robust file creation.

## Purpose and Motivation

The goal of this project is to create a versatile camera solution for advanced applications where both a live video feed and high-quality, unprocessed still images are required. This is particularly useful for:

* **Scientific Imaging & Microscopy:** Using the UVC stream for live viewfinding and focusing, while capturing RAW DNG files for analysis and publication.
* **Computer Vision Data Acquisition:** Streaming a live preview to monitor a scene and programmatically triggering RAW captures for training datasets when an event of interest occurs.
* **Quality Control Systems:** Monitoring a production line with the video stream and capturing detailed still images of products for automated inspection.

## Prerequisites

The project is built using the Meson build system. The following libraries are required:

* `libcamera`
* `libjpeg`
* `libtiff-4`
* A C++17 compliant compiler

## Building the Project

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/yasintuncerr/picam.git
    cd picam
    ```

2.  **Configure the build with Meson:**
    ```bash
    meson setup build
    ```

3.  **Compile with Ninja:**
    ```bash
    ninja -C build
    ```

The compiled binary will be located at `build/picam`.

## Usage

The application is run from the command line. To enable the still capture server, you must use a `libcamera` source (`-c`) and specify a port for the HTTP server (`-p`).

**Example:**

```bash
# Run the application using the first libcamera device (index 0)
# and start the HTTP server on port 8080.
./build/picam -c 0 -p 8080 uvc.0
```

Once running, the device will enumerate as a standard UVC webcam. You can view the video stream using any compatible software (e.g., VLC, OBS, guvcview).

**To trigger a still capture:**

Use a tool like `curl` to make an HTTP GET request to the `/capture` endpoint. The server will respond with the DNG file.

```bash
# Replace <device-ip> with the IP address of the device running picam
curl http://<device-ip>:8080/capture > my_image.dng
```

This will save the captured RAW image as `my_image.dng`.

## Future Work

Currently, the DNG file is generated within the `libcamera` request completion callback (`libcamera_source_still_process`). A planned improvement is to move the DNG writing process to a separate worker thread within the `capture.c` module. This will further decouple the still capture logic from the camera event loop, minimizing any potential latency on the video stream.

## Acknowledgments

This work would not be possible without the excellent foundational projects:

* The original **uvc-gadget** authors and contributors.
* The **libcamera** team for providing a powerful, modern camera stack for Linux.

## License

This project is licensed under the LGPL-2.1-or-later, consistent with the upstream `uvc-gadget` project.