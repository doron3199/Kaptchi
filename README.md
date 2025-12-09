# Kaptchi Flutter

Kaptchi is an assistive technology application designed to help visually impaired students see the whiteboard clearly in a classroom setting.

## Features

*   **Real-time Visual Enhancement:**
    *   **Infinite Zoom:** High-level digital zoom capabilities.
    *   **Color Inversion:** High contrast modes for better visibility.
    *   **Whiteboard Filter:** Enhances text visibility and removes glare/shadows.
*   **Obstacle Removal:** Uses computer vision (temporal median filtering) to "erase" moving objects (like a lecturer) from the view, leaving only the static whiteboard content.
*   **Cross-Device Streaming:**
    *   **Camera Mode (Android):** Uses the smartphone as a wireless camera.
    *   **Monitor Mode (Windows):** Receives the stream, performs heavy image processing (C++), and displays the enhanced video on a larger screen.
*   **Capture & Save:** Take snapshots of the processed whiteboard and save them as PDF notes.

## Architecture

*   **Frontend:** Flutter (Android & Windows).
*   **Image Processing:** C++ via `dart:ffi` (OpenCV) for high-performance filtering and background subtraction.
*   **Streaming:** RTMP for video transmission over a local network (via MediaMTX).
*   **Discovery:** Network interface scanning for local device connection.

## Getting Started

This project is a starting point for a Flutter application.

A few resources to get you started if this is your first Flutter project:

- [Lab: Write your first Flutter app](https://docs.flutter.dev/get-started/codelab)
- [Cookbook: Useful Flutter samples](https://docs.flutter.dev/cookbook)

For help getting started with Flutter development, view the
[online documentation](https://docs.flutter.dev/), which offers tutorials,
samples, guidance on mobile development, and a full API reference.
