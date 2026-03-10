import 'dart:ffi' hide Size;
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui';
import 'package:ffi/ffi.dart';

typedef GetTextureIdFunc = Int64 Function();
typedef GetTextureId = int Function();

typedef StartCameraFunc = Void Function();
typedef StartCamera = void Function();

typedef StartStreamFunc = Void Function(Pointer<Utf8> url);
typedef StartStream = void Function(Pointer<Utf8> url);

typedef StopCameraFunc = Void Function();
typedef StopCamera = void Function();

typedef SwitchCameraFunc = Void Function();
typedef SwitchCamera = void Function();

typedef SelectCameraFunc = Void Function(Int32 index);
typedef SelectCamera = void Function(int index);

typedef SetResolutionFunc = Void Function(Int32 width, Int32 height);
typedef SetResolution = void Function(int width, int height);

typedef SetFilterSequenceFunc =
    Void Function(Pointer<Int32> filters, Int32 count);
typedef SetFilterSequence = void Function(Pointer<Int32> filters, int count);

typedef GetFrameDataFunc = Void Function(Pointer<Uint8> buffer, Int32 size);
typedef GetFrameData = void Function(Pointer<Uint8> buffer, int size);

typedef GetFrameWidthFunc = Int32 Function();
typedef GetFrameWidth = int Function();

typedef GetFrameHeightFunc = Int32 Function();
typedef GetFrameHeight = int Function();

typedef SetLiveCropCornersFunc = Void Function(Pointer<Double> corners);
typedef SetLiveCropCorners = void Function(Pointer<Double> corners);

typedef SetFilterParameterFunc = Void Function(Int32 filterId, Float param1);
typedef SetFilterParameter = void Function(int filterId, double param1);

// Panorama FFI types
typedef SetPanoramaEnabledFunc = Void Function(Bool enabled);
typedef SetPanoramaEnabled = void Function(bool enabled);

typedef ResetPanoramaFunc = Void Function();
typedef ResetPanorama = void Function();

typedef SetPanoramaViewportFunc =
    Void Function(Float panX, Float panY, Float zoom);
typedef SetPanoramaViewport =
    void Function(double panX, double panY, double zoom);

typedef GetPanoramaCanvasSizeFunc =
    Void Function(Pointer<Int32> width, Pointer<Int32> height);
typedef GetPanoramaCanvasSize =
    void Function(Pointer<Int32> width, Pointer<Int32> height);

typedef IsPanoramaEnabledFunc = Bool Function();
typedef IsPanoramaEnabled = bool Function();

// Canvas (whiteboard) FFI types
typedef SetCanvasViewModeFunc = Void Function(Bool mode);
typedef SetCanvasViewMode = void Function(bool mode);

typedef IsCanvasViewModeFunc = Bool Function();
typedef IsCanvasViewMode = bool Function();

typedef SetCanvasRenderModeFunc = Void Function(Int32 mode);
typedef SetCanvasRenderMode = void Function(int mode);

typedef GetCanvasTextureIdFunc = Int64 Function();
typedef GetCanvasTextureId = int Function();

typedef GetCanvasOverviewRgbaFunc = Bool Function(
  Pointer<Uint8> buffer,
  Int32 width,
  Int32 height,
);
typedef GetCanvasOverviewRgba = bool Function(
  Pointer<Uint8> buffer,
  int width,
  int height,
);

typedef SetWhiteboardDebugFunc = Void Function(Bool enabled);
typedef SetWhiteboardDebug = void Function(bool enabled);

typedef SetCanvasEnhanceThresholdFunc = Void Function(Float threshold);
typedef SetCanvasEnhanceThreshold = void Function(double threshold);

// Sub-canvas navigation FFI types
typedef GetSubCanvasCountFunc = Int32 Function();
typedef GetSubCanvasCount = int Function();

typedef GetActiveSubCanvasIndexFunc = Int32 Function();
typedef GetActiveSubCanvasIndex = int Function();

typedef SetActiveSubCanvasFunc = Void Function(Int32 idx);
typedef SetActiveSubCanvas = void Function(int idx);

typedef GetSortedSubCanvasIndexFunc = Int32 Function(Int32 pos);
typedef GetSortedSubCanvasIndex = int Function(int pos);

typedef GetSortedPositionFunc = Int32 Function(Int32 idx);
typedef GetSortedPosition = int Function(int idx);

class NativeCameraService {
  static final NativeCameraService _instance = NativeCameraService._internal();
  factory NativeCameraService() => _instance;
  NativeCameraService._internal();

  late DynamicLibrary _nativeLib;
  late GetTextureId _getTextureId;
  late StartCamera _startCamera;
  late StartStream _startStream;
  late StopCamera _stopCamera;
  late SwitchCamera _switchCamera;
  late SelectCamera _selectCamera;
  late SetResolution _setResolution;
  late SetFilterSequence _setFilterSequence;
  late GetFrameData _getFrameData;
  late GetFrameWidth _getFrameWidth;
  late GetFrameHeight _getFrameHeight;
  late SetLiveCropCorners _setLiveCropCorners;
  late SetFilterParameter _setFilterParameter;

  // Panorama bindings
  late SetPanoramaEnabled _setPanoramaEnabled;
  late ResetPanorama _resetPanorama;
  late SetPanoramaViewport _setPanoramaViewport;
  late GetPanoramaCanvasSize _getPanoramaCanvasSize;
  late IsPanoramaEnabled _isPanoramaEnabled;

  // Canvas (whiteboard) bindings
  late SetCanvasViewMode _setCanvasViewMode;
  late IsCanvasViewMode _isCanvasViewMode;
  late SetCanvasRenderMode _setCanvasRenderMode;
  late GetCanvasTextureId _getCanvasTextureId;
  late GetCanvasOverviewRgba _getCanvasOverviewRgba;
  late SetWhiteboardDebug _setWhiteboardDebug;
  late SetCanvasEnhanceThreshold _setCanvasEnhanceThreshold;

  // Sub-canvas navigation bindings
  late GetSubCanvasCount _getSubCanvasCount;
  late GetActiveSubCanvasIndex _getActiveSubCanvasIndex;
  late SetActiveSubCanvas _setActiveSubCanvas;
  late GetSortedSubCanvasIndex _getSortedSubCanvasIndex;
  late GetSortedPosition _getSortedPosition;

  bool _isInitialized = false;

  void initialize() {
    if (_isInitialized) return;

    if (Platform.isWindows) {
      // The library is linked into the executable, so we open the current process
      _nativeLib = DynamicLibrary.process();
    } else {
      throw UnsupportedError('Native camera only supported on Windows');
    }

    _getTextureId = _nativeLib
        .lookup<NativeFunction<GetTextureIdFunc>>('GetTextureId')
        .asFunction();
    _startCamera = _nativeLib
        .lookup<NativeFunction<StartCameraFunc>>('StartCamera')
        .asFunction();
    _startStream = _nativeLib
        .lookup<NativeFunction<StartStreamFunc>>('StartStream')
        .asFunction();
    _stopCamera = _nativeLib
        .lookup<NativeFunction<StopCameraFunc>>('StopCamera')
        .asFunction();
    _switchCamera = _nativeLib
        .lookup<NativeFunction<SwitchCameraFunc>>('SwitchCamera')
        .asFunction();
    _selectCamera = _nativeLib
        .lookup<NativeFunction<SelectCameraFunc>>('SelectCamera')
        .asFunction();
    _setResolution = _nativeLib
        .lookup<NativeFunction<SetResolutionFunc>>('SetResolution')
        .asFunction();
    _setFilterSequence = _nativeLib
        .lookup<NativeFunction<SetFilterSequenceFunc>>('SetFilterSequence')
        .asFunction();

    _getFrameData = _nativeLib
        .lookup<NativeFunction<GetFrameDataFunc>>('GetFrameData')
        .asFunction();
    _getFrameWidth = _nativeLib
        .lookup<NativeFunction<GetFrameWidthFunc>>('GetFrameWidth')
        .asFunction();
    _getFrameHeight = _nativeLib
        .lookup<NativeFunction<GetFrameHeightFunc>>('GetFrameHeight')
        .asFunction();
    _setLiveCropCorners = _nativeLib
        .lookup<NativeFunction<SetLiveCropCornersFunc>>('SetLiveCropCorners')
        .asFunction();
    _setFilterParameter = _nativeLib
        .lookup<NativeFunction<SetFilterParameterFunc>>('SetFilterParameter')
        .asFunction();

    // Panorama bindings
    _setPanoramaEnabled = _nativeLib
        .lookup<NativeFunction<SetPanoramaEnabledFunc>>('SetPanoramaEnabled')
        .asFunction();
    _resetPanorama = _nativeLib
        .lookup<NativeFunction<ResetPanoramaFunc>>('ResetPanorama')
        .asFunction();
    _setPanoramaViewport = _nativeLib
        .lookup<NativeFunction<SetPanoramaViewportFunc>>('SetPanoramaViewport')
        .asFunction();
    _getPanoramaCanvasSize = _nativeLib
        .lookup<NativeFunction<GetPanoramaCanvasSizeFunc>>(
          'GetPanoramaCanvasSize',
        )
        .asFunction();
    _isPanoramaEnabled = _nativeLib
        .lookup<NativeFunction<IsPanoramaEnabledFunc>>('IsPanoramaEnabled')
        .asFunction();

    // Canvas (whiteboard) bindings
    _setCanvasViewMode = _nativeLib
        .lookup<NativeFunction<SetCanvasViewModeFunc>>('SetCanvasViewMode')
        .asFunction();
    _isCanvasViewMode = _nativeLib
        .lookup<NativeFunction<IsCanvasViewModeFunc>>('IsCanvasViewMode')
        .asFunction();
    _setCanvasRenderMode = _nativeLib
      .lookup<NativeFunction<SetCanvasRenderModeFunc>>('SetCanvasRenderMode')
      .asFunction();
    _getCanvasTextureId = _nativeLib
        .lookup<NativeFunction<GetCanvasTextureIdFunc>>('GetCanvasTextureId')
        .asFunction();
    _getCanvasOverviewRgba = _nativeLib
      .lookup<NativeFunction<GetCanvasOverviewRgbaFunc>>(
        'GetCanvasOverviewRgba',
      )
      .asFunction();
    _setWhiteboardDebug = _nativeLib
        .lookup<NativeFunction<SetWhiteboardDebugFunc>>('SetWhiteboardDebug')
        .asFunction();
    _setCanvasEnhanceThreshold = _nativeLib
        .lookup<NativeFunction<SetCanvasEnhanceThresholdFunc>>(
          'SetCanvasEnhanceThreshold',
        )
        .asFunction();

    // Sub-canvas navigation bindings
    _getSubCanvasCount = _nativeLib
        .lookup<NativeFunction<GetSubCanvasCountFunc>>('GetSubCanvasCount')
        .asFunction();
    _getActiveSubCanvasIndex = _nativeLib
        .lookup<NativeFunction<GetActiveSubCanvasIndexFunc>>(
          'GetActiveSubCanvasIndex',
        )
        .asFunction();
    _setActiveSubCanvas = _nativeLib
        .lookup<NativeFunction<SetActiveSubCanvasFunc>>('SetActiveSubCanvas')
        .asFunction();
    _getSortedSubCanvasIndex = _nativeLib
        .lookup<NativeFunction<GetSortedSubCanvasIndexFunc>>(
          'GetSortedSubCanvasIndex',
        )
        .asFunction();
    _getSortedPosition = _nativeLib
        .lookup<NativeFunction<GetSortedPositionFunc>>('GetSortedPosition')
        .asFunction();

    _isInitialized = true;
  }

  int getTextureId() {
    initialize();
    return _getTextureId();
  }

  void start() {
    initialize();
    _startCamera();
  }

  void startStream(String url) {
    initialize();
    final ptr = url.toNativeUtf8();
    try {
      _startStream(ptr);
    } finally {
      malloc.free(ptr);
    }
  }

  void stop() {
    initialize();
    _stopCamera();
  }

  void switchCamera() {
    initialize();
    _switchCamera();
  }

  void selectCamera(int index) {
    initialize();
    _selectCamera(index);
  }

  void setResolution(int width, int height) {
    initialize();
    _setResolution(width, height);
  }

  void setFilterSequence(List<int> filters) {
    initialize();
    final ptr = malloc.allocate<Int32>(filters.length * 4);
    try {
      final list = ptr.asTypedList(filters.length);
      list.setAll(0, filters);
      _setFilterSequence(ptr, filters.length);
    } finally {
      malloc.free(ptr);
    }
  }

  int getFrameWidth() {
    initialize();
    return _getFrameWidth();
  }

  int getFrameHeight() {
    initialize();
    return _getFrameHeight();
  }

  Uint8List? getFrameData() {
    initialize();
    final width = _getFrameWidth();
    final height = _getFrameHeight();
    if (width == 0 || height == 0) return null;

    final size = width * height * 4;
    final ptr = malloc.allocate<Uint8>(size);
    try {
      _getFrameData(ptr, size);
      return Uint8List.fromList(ptr.asTypedList(size));
    } finally {
      malloc.free(ptr);
    }
  }

  /// Set or clear live perspective crop corners.
  /// [corners] should be 4 Offsets in order: TL, TR, BR, BL (normalized 0..1).
  /// Pass null to disable live crop.
  void setLiveCropCorners(List<Offset>? corners) {
    initialize();
    if (corners == null) {
      _setLiveCropCorners(nullptr);
    } else {
      // Allocate 8 doubles (4 corners × 2 coordinates)
      final ptr = malloc.allocate<Double>(8 * 8); // 8 doubles
      try {
        for (int i = 0; i < 4; i++) {
          ptr[i * 2] = corners[i].dx;
          ptr[i * 2 + 1] = corners[i].dy;
        }
        _setLiveCropCorners(ptr);
      } finally {
        malloc.free(ptr);
      }
    }
  }

  /// Set a parameter value for a specific filter
  void setFilterParameter(int filterId, double value) {
    initialize();
    _setFilterParameter(filterId, value);
  }

  // --- Panorama Methods ---

  /// Enable or disable panorama mode
  void setPanoramaEnabled(bool enabled) {
    initialize();
    _setPanoramaEnabled(enabled);
  }

  /// Reset the panorama canvas
  void resetPanorama() {
    initialize();
    _resetPanorama();
  }

  /// Set panorama viewport parameters
  /// [panX], [panY]: normalized position (0.0 = left/top, 1.0 = right/bottom)
  /// [zoom]: 1.0 = fit viewport, >1.0 = zoomed in
  void setPanoramaViewport(double panX, double panY, double zoom) {
    initialize();
    _setPanoramaViewport(panX, panY, zoom);
  }

  /// Get the current panorama canvas size
  Size getPanoramaCanvasSize() {
    initialize();
    final widthPtr = malloc.allocate<Int32>(4);
    final heightPtr = malloc.allocate<Int32>(4);
    try {
      _getPanoramaCanvasSize(widthPtr, heightPtr);
      return Size(widthPtr.value.toDouble(), heightPtr.value.toDouble());
    } finally {
      malloc.free(widthPtr);
      malloc.free(heightPtr);
    }
  }

  /// Check if panorama mode is enabled
  bool isPanoramaEnabled() {
    initialize();
    return _isPanoramaEnabled();
  }

  // --- Canvas (Whiteboard) Methods ---

  /// Enable or disable the canvas view (shows assembled canvas instead of live feed)
  void setCanvasViewMode(bool mode) {
    initialize();
    _setCanvasViewMode(mode);
  }

  /// Returns true when the canvas viewport is currently displayed
  bool isCanvasViewMode() {
    initialize();
    return _isCanvasViewMode();
  }

  /// Select which stitched whiteboard output to display.
  /// [mode]: 0 = stroke view, 1 = raw native-frame mosaic.
  void setCanvasRenderMode(int mode) {
    initialize();
    _setCanvasRenderMode(mode);
  }

  /// Returns the Flutter texture ID for the canvas (Phase 2; -1 = not available)
  int getCanvasTextureId() {
    initialize();
    return _getCanvasTextureId();
  }

  /// Returns a low-resolution RGBA overview of the full canvas, or null if unavailable.
  Uint8List? getCanvasOverviewRgba(int width, int height) {
    initialize();
    if (width <= 0 || height <= 0) return null;

    final size = width * height * 4;
    final ptr = malloc.allocate<Uint8>(size);
    try {
      final ok = _getCanvasOverviewRgba(ptr, width, height);
      if (!ok) return null;
      return Uint8List.fromList(ptr.asTypedList(size));
    } finally {
      malloc.free(ptr);
    }
  }

  /// Toggle OpenCV debug popup windows for the whiteboard pipeline
  void setWhiteboardDebug(bool enabled) {
    initialize();
    _setWhiteboardDebug(enabled);
  }

  /// Set the DoG noise-suppression threshold for WhiteboardEnhance (1–30).
  void setCanvasEnhanceThreshold(double threshold) {
    initialize();
    _setCanvasEnhanceThreshold(threshold);
  }

  // --- Sub-canvas Navigation Methods ---

  /// Returns the number of active sub-canvases
  int getSubCanvasCount() {
    initialize();
    return _getSubCanvasCount();
  }

  /// Returns the index of the currently displayed sub-canvas (-1 if none)
  int getActiveSubCanvasIndex() {
    initialize();
    return _getActiveSubCanvasIndex();
  }

  /// Switch the displayed sub-canvas to [idx]
  void setActiveSubCanvas(int idx) {
    initialize();
    _setActiveSubCanvas(idx);
  }

  /// Return the vector index of the sub-canvas at sorted position [pos]
  /// (sorted spatially by origin.y then origin.x). Returns -1 if out of range.
  int getSortedSubCanvasIndex(int pos) {
    initialize();
    return _getSortedSubCanvasIndex(pos);
  }

  /// Return the sorted position of the given vector index. Returns -1 if invalid.
  int getSortedPosition(int idx) {
    initialize();
    return _getSortedPosition(idx);
  }

  // --- Screen Capture Methods ---

  late GetWindowCount _getWindowCount;
  late GetWindowTitle _getWindowTitle;
  late GetWindowHandle _getWindowHandle;
  late StartScreenCapture _startScreenCapture;
  late StopScreenCapture _stopScreenCapture;
  late IsScreenCaptureActive _isScreenCaptureActive;
  late GetMonitorCount _getMonitorCount;
  late GetMonitorName _getMonitorName;
  late GetMonitorBounds _getMonitorBounds;

  bool _screenCaptureInitialized = false;

  void _initializeScreenCapture() {
    if (_screenCaptureInitialized) return;
    initialize();

    _getWindowCount = _nativeLib
        .lookup<NativeFunction<GetWindowCountFunc>>('GetWindowCount')
        .asFunction();
    _getWindowTitle = _nativeLib
        .lookup<NativeFunction<GetWindowTitleFunc>>('GetWindowTitle')
        .asFunction();
    _getWindowHandle = _nativeLib
        .lookup<NativeFunction<GetWindowHandleFunc>>('GetWindowHandle')
        .asFunction();
    _startScreenCapture = _nativeLib
        .lookup<NativeFunction<StartScreenCaptureFunc>>('StartScreenCapture')
        .asFunction();
    _stopScreenCapture = _nativeLib
        .lookup<NativeFunction<StopScreenCaptureFunc>>('StopScreenCapture')
        .asFunction();
    _isScreenCaptureActive = _nativeLib
        .lookup<NativeFunction<IsScreenCaptureActiveFunc>>(
          'IsScreenCaptureActive',
        )
        .asFunction();
    _getMonitorCount = _nativeLib
        .lookup<NativeFunction<GetMonitorCountFunc>>('GetMonitorCount')
        .asFunction();
    _getMonitorName = _nativeLib
        .lookup<NativeFunction<GetMonitorNameFunc>>('GetMonitorName')
        .asFunction();
    _getMonitorBounds = _nativeLib
        .lookup<NativeFunction<GetMonitorBoundsFunc>>('GetMonitorBounds')
        .asFunction();

    _screenCaptureInitialized = true;
  }

  /// Get list of capturable windows
  List<WindowInfo> getCapturableWindows() {
    _initializeScreenCapture();
    final count = _getWindowCount();
    final windows = <WindowInfo>[];

    for (int i = 0; i < count; i++) {
      // Get title
      final buffer = malloc.allocate<Utf8>(512);
      try {
        final len = _getWindowTitle(i, buffer.cast<Char>(), 512);
        if (len > 0) {
          final title = buffer.toDartString(length: len);
          final handle = _getWindowHandle(i);
          windows.add(WindowInfo(handle: handle, title: title));
        }
      } finally {
        malloc.free(buffer);
      }
    }

    return windows;
  }

  /// Get list of available monitors
  List<MonitorInfo> getMonitors() {
    _initializeScreenCapture();
    final count = _getMonitorCount();
    final monitors = <MonitorInfo>[];

    // Allocate pointers for bounds
    final leftPtr = malloc.allocate<Int32>(4);
    final topPtr = malloc.allocate<Int32>(4);
    final rightPtr = malloc.allocate<Int32>(4);
    final bottomPtr = malloc.allocate<Int32>(4);

    try {
      for (int i = 0; i < count; i++) {
        final buffer = malloc.allocate<Utf8>(256);
        try {
          final len = _getMonitorName(i, buffer.cast<Char>(), 256);
          if (len > 0) {
            final name = buffer.toDartString(length: len);

            // Get monitor bounds
            _getMonitorBounds(i, leftPtr, topPtr, rightPtr, bottomPtr);
            final left = leftPtr.value;
            final top = topPtr.value;
            final right = rightPtr.value;
            final bottom = bottomPtr.value;
            final width = right - left;
            final height = bottom - top;
            // Primary monitor is at (0,0)
            final isPrimary = (left == 0 && top == 0);

            monitors.add(
              MonitorInfo(
                index: i,
                name: name,
                width: width,
                height: height,
                isPrimary: isPrimary,
              ),
            );
          }
        } finally {
          malloc.free(buffer);
        }
      }
    } finally {
      malloc.free(leftPtr);
      malloc.free(topPtr);
      malloc.free(rightPtr);
      malloc.free(bottomPtr);
    }

    return monitors;
  }

  /// Start capturing a monitor (0 = full screen, windowHandle = specific window)
  bool startScreenCapture(int monitorIndex, {int windowHandle = 0}) {
    _initializeScreenCapture();
    return _startScreenCapture(monitorIndex, windowHandle) == 1;
  }

  /// Stop screen capture
  void stopScreenCapture() {
    _initializeScreenCapture();
    _stopScreenCapture();
  }

  /// Check if screen capture is currently active
  bool isScreenCaptureActive() {
    _initializeScreenCapture();
    return _isScreenCaptureActive() == 1;
  }
}

/// Window info for screen capture
class WindowInfo {
  final int handle;
  final String title;

  WindowInfo({required this.handle, required this.title});

  @override
  String toString() => title;
}

/// Monitor info for screen capture
class MonitorInfo {
  final int index;
  final String name;
  final int width;
  final int height;
  final bool isPrimary;

  MonitorInfo({
    required this.index,
    required this.name,
    required this.width,
    required this.height,
    required this.isPrimary,
  });

  @override
  String toString() => name;
}

// FFI type definitions for screen capture
typedef GetWindowCountFunc = Int32 Function();
typedef GetWindowCount = int Function();

typedef GetWindowTitleFunc =
    Int32 Function(Int32 index, Pointer<Char> buffer, Int32 bufferSize);
typedef GetWindowTitle =
    int Function(int index, Pointer<Char> buffer, int bufferSize);

typedef GetWindowHandleFunc = Int64 Function(Int32 index);
typedef GetWindowHandle = int Function(int index);

typedef StartScreenCaptureFunc =
    Int32 Function(Int32 monitorIndex, Int64 windowHandle);
typedef StartScreenCapture = int Function(int monitorIndex, int windowHandle);

typedef GetMonitorCountFunc = Int32 Function();
typedef GetMonitorCount = int Function();

typedef GetMonitorNameFunc =
    Int32 Function(Int32 index, Pointer<Char> buffer, Int32 bufferSize);
typedef GetMonitorName =
    int Function(int index, Pointer<Char> buffer, int bufferSize);

typedef GetMonitorBoundsFunc =
    Void Function(
      Int32 index,
      Pointer<Int32> left,
      Pointer<Int32> top,
      Pointer<Int32> right,
      Pointer<Int32> bottom,
    );
typedef GetMonitorBounds =
    void Function(
      int index,
      Pointer<Int32> left,
      Pointer<Int32> top,
      Pointer<Int32> right,
      Pointer<Int32> bottom,
    );

typedef StopScreenCaptureFunc = Void Function();
typedef StopScreenCapture = void Function();

typedef IsScreenCaptureActiveFunc = Int32 Function();
typedef IsScreenCaptureActive = int Function();
