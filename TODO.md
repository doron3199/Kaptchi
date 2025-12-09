# Kaptchi Development Plan

## Phase 1: Setup & Infrastructure
- [ ] **Project Configuration**: Add dependencies (`camera`, `haishin_kit`, `path_provider`, `pdf`, `ffi`).
- [ ] **Permissions**: Configure Android (Camera, Internet) and Windows (Private Networks) permissions.
- [ ] **Media Server**: Implement a local RTMP server (MediaMTX) within the Windows app to facilitate streaming connections.

## Phase 2: Core Streaming (RTMP)
- [ ] **Android "Camera Mode"**: Create UI and logic to capture camera feed and stream to the Windows host via RTMP.
- [ ] **Windows "Monitor Mode"**: Create UI to host the session, accept connections, and render the incoming video stream.
- [ ] **Network Discovery**: Implement IP entry for connecting devices.
- [ ] **QR Code Connection**: Display a QR code in Monitor Mode containing the IP; Camera Mode scans it to auto-connect.

## Phase 3: Image Processing (C++ & FFI)
- [x] **C++ Integration**: Set up the C++ build environment (CMake) and `dart:ffi` bindings.
- [x] **Frame Extraction**: Implement logic to extract raw pixel data from the stream (or local camera) for processing.
- [x] **Basic Filters**: Implement "Color Inversion" and "Whiteboard Enhancement" (Adaptive Thresholding) in C++.
- [x] **Obstacle Removal**: Implement the "Temporal Median Filter" in C++ to remove moving objects from the static background.

## Phase 4: UI & Features
- [x] **Infinite Zoom**: Implement a zoomable interactive viewer for the video feed.
- [x] **Filter Controls**: Add UI toggles for the different enhancement modes.
- [ ] **PDF Export**: Implement functionality to capture the current *processed* frame and save it as a PDF file.

## Phase 5: Optimization & Polish
- [ ] **Performance Tuning**: Optimize C++ processing pipeline for real-time frame rates.
- [ ] **Battery Optimization**: Ensure the Android "Camera Mode" is efficient.
- [ ] **Testing**: Verify connectivity and stability across different network conditions.
