import 'dart:ffi';
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
