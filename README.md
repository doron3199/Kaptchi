# Kaptchi: The Ultimate Vision Aid for Students

**Kaptchi** is a powerful, open-source application designed to help **visually impaired students** see the whiteboard better. It transforms your computer into a personal vision tool by allowing you to seamlessly switch between multiple cameras, screen capture, and apply real-time high-contrast enhancements.

## Key Features

- **Vision Accessibility**: High-contrast "Whiteboard Mode" and sharpening filters to make text legible.
- **Multi-Camera Support**: Connect and switch between multiple webcams instantly.
- **Screen Capture**: Capture specific windows or entire screens (e.g., Zoom/Teams shared screen) and apply filters to them.
- **Real-Time Filters**:
    - **Whiteboard Mode**: High-contrast filter to make whiteboard writing pop.
    - **Stabilization**: Smooth out camera shake.
    - **Sharpening & Adjustments**: Invert colors, adjust brightness, and more.
- **PDF Export**: Snap pictures during class and export them as a single PDF document.
- **Mobile Connection**: Use your smartphone as a portable camera close to the board via QR code connection.

> **Tip**: If you have a single monitor setup and want to screen capture apps like Zoom or Google Meet without the app window blocking your view, we recommend installing the [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) to create a virtual second monitor.

## Getting Started

### Prerequisites

- Windows 10/11
- Webcam (Optional, but recommended)

### Installation

1.  Download the latest release from the [Releases](https://github.com/doron3199/kaptchi_flutter/releases) page.
2.  Extract the zip file.
3.  Run `kaptchi_flutter.exe`.
4.  Install the android app from the app store.

### Development Setup

To build Kaptchi from source, you need Flutter installed.

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/doron3199/kaptchi_flutter.git
    cd kaptchi_flutter
    ```

2.  **Install dependencies:**
    ```bash
    flutter pub get
    ```

3.  **Run the app:**
    ```bash
    flutter run -d windows
    ```

## Usage Guide

1.  **Connect Sources**: Plug in your webcams. Kaptchi will automatically detect them.
2.  **Select Source**: Click on a camera preview in the left sidebar to make it the main view.
3.  **Apply Filters**: Open the right sidebar (filters icon) to toggle stabilization, whiteboard mode, and other effects.
4.  **Capture**: Use the shutter button to take snapshots.
5.  **Export**: Click the PDF icon in the gallery sidebar to save your captured images as a PDF.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Notice

The Majority is build with the help of AI. Mainly Gemini 3 pro and Claude 4.5 Opus
There can be mistakes in the code, please report them if you find any.
