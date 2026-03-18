import 'dart:ffi' hide Size;
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui';
import 'package:ffi/ffi.dart';
import '../models/graph_node_info.dart';
import 'app_logger.dart';

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

// Pipeline mode FFI types
typedef SetCanvasPipelineModeFunc = Void Function(Int32 mode);
typedef SetCanvasPipelineMode = void Function(int mode);

typedef GetCanvasPipelineModeFunc = Int32 Function();
typedef GetCanvasPipelineMode = int Function();

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

// Graph debug FFI types
typedef GetGraphNodeCountFunc = Int32 Function();
typedef GetGraphNodeCount = int Function();

typedef GetGraphNodesFunc = Int32 Function(Pointer<Float> buffer, Int32 maxNodes);
typedef GetGraphNodesFFI = int Function(Pointer<Float> buffer, int maxNodes);

typedef GetGraphNodeNeighborsFunc = Int32 Function(
  Int32 nodeId, Pointer<Int32> neighbors, Int32 maxNeighbors);
typedef GetGraphNodeNeighborsFFI = int Function(
  int nodeId, Pointer<Int32> neighbors, int maxNeighbors);

typedef CompareGraphNodesFunc = Bool Function(
  Int32 idA, Int32 idB, Pointer<Float> result);
typedef CompareGraphNodesFFI = bool Function(
  int idA, int idB, Pointer<Float> result);

typedef MoveGraphNodeFunc = Bool Function(
  Int32 nodeId, Float newCx, Float newCy);
typedef MoveGraphNodeFFI = bool Function(
  int nodeId, double newCx, double newCy);

typedef GetGraphCanvasBoundsFunc = Bool Function(Pointer<Int32> bounds);
typedef GetGraphCanvasBoundsFFI = bool Function(Pointer<Int32> bounds);

typedef GetGraphNodeContoursFunc = Int32 Function(Pointer<Float> buffer, Int32 maxFloats);
typedef GetGraphNodeContoursFFI = int Function(Pointer<Float> buffer, int maxFloats);

typedef CaptureGraphDebugSnapshotFunc = Bool Function(Int32 slot);
typedef CaptureGraphDebugSnapshotFFI = bool Function(int slot);

typedef GetGraphSnapshotNodeCountFunc = Int32 Function(Int32 slot);
typedef GetGraphSnapshotNodeCountFFI = int Function(int slot);

typedef GetGraphSnapshotNodesFunc = Int32 Function(
  Int32 slot, Pointer<Float> buffer, Int32 maxNodes);
typedef GetGraphSnapshotNodesFFI = int Function(
  int slot, Pointer<Float> buffer, int maxNodes);

typedef GetGraphSnapshotCanvasBoundsFunc = Bool Function(
  Int32 slot, Pointer<Int32> bounds);
typedef GetGraphSnapshotCanvasBoundsFFI = bool Function(
  int slot, Pointer<Int32> bounds);

typedef GetGraphSnapshotNodeContoursFunc = Int32 Function(
  Int32 slot, Pointer<Float> buffer, Int32 maxFloats);
typedef GetGraphSnapshotNodeContoursFFI = int Function(
  int slot, Pointer<Float> buffer, int maxFloats);

typedef CompareGraphSnapshotNodesFunc = Bool Function(
  Int32 slotA,
  Int32 idA,
  Int32 slotB,
  Int32 idB,
  Pointer<Float> result,
);
typedef CompareGraphSnapshotNodesFFI = bool Function(
  int slotA,
  int idA,
  int slotB,
  int idB,
  Pointer<Float> result,
);

typedef CombineGraphDebugSnapshotsFunc = Bool Function(
  Int32 slotA,
  Int32 anchorIdA,
  Int32 slotB,
  Int32 anchorIdB,
);
typedef CombineGraphDebugSnapshotsFFI = bool Function(
  int slotA,
  int anchorIdA,
  int slotB,
  int anchorIdB,
);

typedef CopyGraphDebugSnapshotFunc = Bool Function(Int32 sourceSlot, Int32 targetSlot);
typedef CopyGraphDebugSnapshotFFI = bool Function(int sourceSlot, int targetSlot);

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
  late SetCanvasPipelineMode _setCanvasPipelineMode;
  late GetCanvasPipelineMode _getCanvasPipelineMode;

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
    _setCanvasPipelineMode = _nativeLib
        .lookup<NativeFunction<SetCanvasPipelineModeFunc>>(
          'SetCanvasPipelineMode',
        )
        .asFunction();
    _getCanvasPipelineMode = _nativeLib
        .lookup<NativeFunction<GetCanvasPipelineModeFunc>>(
          'GetCanvasPipelineMode',
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

  /// Set the canvas pipeline mode (0 = Graph, 1 = Chunk).
  /// Switching mode resets the canvas.
  void setCanvasPipelineMode(int mode) {
    initialize();
    _setCanvasPipelineMode(mode);
  }

  /// Get the current canvas pipeline mode (0 = Graph, 1 = Chunk).
  int getCanvasPipelineMode() {
    initialize();
    return _getCanvasPipelineMode();
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

  // --- Graph Debug Methods ---

  late GetGraphNodeCount _getGraphNodeCount;
  late GetGraphNodesFFI _getGraphNodes;
  late GetGraphNodeNeighborsFFI _getGraphNodeNeighbors;
  late CompareGraphNodesFFI _compareGraphNodes;
  late MoveGraphNodeFFI _moveGraphNode;
  late GetGraphCanvasBoundsFFI _getGraphCanvasBounds;
  GetGraphNodeContoursFFI? _getGraphNodeContours;
  late CaptureGraphDebugSnapshotFFI _captureGraphDebugSnapshot;
  late GetGraphSnapshotNodeCountFFI _getGraphSnapshotNodeCount;
  late GetGraphSnapshotNodesFFI _getGraphSnapshotNodes;
  late GetGraphSnapshotCanvasBoundsFFI _getGraphSnapshotCanvasBounds;
  GetGraphSnapshotNodeContoursFFI? _getGraphSnapshotNodeContours;
  late CompareGraphSnapshotNodesFFI _compareGraphSnapshotNodes;
  late CombineGraphDebugSnapshotsFFI _combineGraphDebugSnapshots;
  late CopyGraphDebugSnapshotFFI _copyGraphDebugSnapshot;

  bool _graphDebugInitialized = false;

  void _initializeGraphDebug() {
    if (_graphDebugInitialized) return;
    AppLogger.ffi('_initializeGraphDebug: starting, _isInitialized=$_isInitialized');
    try {
      initialize();
    } catch (e) {
      AppLogger.ffi('_initializeGraphDebug: initialize() threw: $e');
      rethrow;
    }
    AppLogger.ffi('_initializeGraphDebug: base initialize() done');

    try {
      _getGraphNodeCount = _nativeLib
          .lookup<NativeFunction<GetGraphNodeCountFunc>>('GetGraphNodeCount')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphNodeCount: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphNodeCount FAILED: $e');
      rethrow;
    }
    try {
      _getGraphNodes = _nativeLib
          .lookup<NativeFunction<GetGraphNodesFunc>>('GetGraphNodes')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphNodes: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphNodes FAILED: $e');
      rethrow;
    }
    try {
      _getGraphNodeNeighbors = _nativeLib
          .lookup<NativeFunction<GetGraphNodeNeighborsFunc>>('GetGraphNodeNeighbors')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphNodeNeighbors: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphNodeNeighbors FAILED: $e');
      rethrow;
    }
    try {
      _compareGraphNodes = _nativeLib
          .lookup<NativeFunction<CompareGraphNodesFunc>>('CompareGraphNodes')
          .asFunction();
      AppLogger.ffi('  lookup CompareGraphNodes: OK');
    } catch (e) {
      AppLogger.ffi('  lookup CompareGraphNodes FAILED: $e');
      rethrow;
    }
    try {
      _moveGraphNode = _nativeLib
          .lookup<NativeFunction<MoveGraphNodeFunc>>('MoveGraphNode')
          .asFunction();
      AppLogger.ffi('  lookup MoveGraphNode: OK');
    } catch (e) {
      AppLogger.ffi('  lookup MoveGraphNode FAILED: $e');
      rethrow;
    }
    try {
      _getGraphCanvasBounds = _nativeLib
          .lookup<NativeFunction<GetGraphCanvasBoundsFunc>>('GetGraphCanvasBounds')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphCanvasBounds: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphCanvasBounds FAILED: $e');
      rethrow;
    }

    // Optional: may not exist if C++ DLL hasn't been rebuilt
    try {
      _getGraphNodeContours = _nativeLib
          .lookup<NativeFunction<GetGraphNodeContoursFunc>>('GetGraphNodeContours')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphNodeContours: OK');
    } catch (e) {
      _getGraphNodeContours = null;
      AppLogger.ffi('  lookup GetGraphNodeContours: not found (optional) - $e');
    }

    try {
      _captureGraphDebugSnapshot = _nativeLib
          .lookup<NativeFunction<CaptureGraphDebugSnapshotFunc>>(
            'CaptureGraphDebugSnapshot',
          )
          .asFunction();
      AppLogger.ffi('  lookup CaptureGraphDebugSnapshot: OK');
    } catch (e) {
      AppLogger.ffi('  lookup CaptureGraphDebugSnapshot FAILED: $e');
      rethrow;
    }
    try {
      _getGraphSnapshotNodeCount = _nativeLib
          .lookup<NativeFunction<GetGraphSnapshotNodeCountFunc>>(
            'GetGraphSnapshotNodeCount',
          )
          .asFunction();
      AppLogger.ffi('  lookup GetGraphSnapshotNodeCount: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphSnapshotNodeCount FAILED: $e');
      rethrow;
    }
    try {
      _getGraphSnapshotNodes = _nativeLib
          .lookup<NativeFunction<GetGraphSnapshotNodesFunc>>(
            'GetGraphSnapshotNodes',
          )
          .asFunction();
      AppLogger.ffi('  lookup GetGraphSnapshotNodes: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphSnapshotNodes FAILED: $e');
      rethrow;
    }
    try {
      _getGraphSnapshotCanvasBounds = _nativeLib
          .lookup<NativeFunction<GetGraphSnapshotCanvasBoundsFunc>>(
            'GetGraphSnapshotCanvasBounds',
          )
          .asFunction();
      AppLogger.ffi('  lookup GetGraphSnapshotCanvasBounds: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphSnapshotCanvasBounds FAILED: $e');
      rethrow;
    }
    try {
      _getGraphSnapshotNodeContours = _nativeLib
          .lookup<NativeFunction<GetGraphSnapshotNodeContoursFunc>>(
            'GetGraphSnapshotNodeContours',
          )
          .asFunction();
      AppLogger.ffi('  lookup GetGraphSnapshotNodeContours: OK');
    } catch (e) {
      _getGraphSnapshotNodeContours = null;
      AppLogger.ffi(
        '  lookup GetGraphSnapshotNodeContours: not found (optional) - $e',
      );
    }
    try {
      _compareGraphSnapshotNodes = _nativeLib
          .lookup<NativeFunction<CompareGraphSnapshotNodesFunc>>(
            'CompareGraphSnapshotNodes',
          )
          .asFunction();
      AppLogger.ffi('  lookup CompareGraphSnapshotNodes: OK');
    } catch (e) {
      AppLogger.ffi('  lookup CompareGraphSnapshotNodes FAILED: $e');
      rethrow;
    }
    try {
      _combineGraphDebugSnapshots = _nativeLib
          .lookup<NativeFunction<CombineGraphDebugSnapshotsFunc>>(
            'CombineGraphDebugSnapshots',
          )
          .asFunction();
      AppLogger.ffi('  lookup CombineGraphDebugSnapshots: OK');
    } catch (e) {
      AppLogger.ffi('  lookup CombineGraphDebugSnapshots FAILED: $e');
      rethrow;
    }
    try {
      _copyGraphDebugSnapshot = _nativeLib
          .lookup<NativeFunction<CopyGraphDebugSnapshotFunc>>(
            'CopyGraphDebugSnapshot',
          )
          .asFunction();
      AppLogger.ffi('  lookup CopyGraphDebugSnapshot: OK');
    } catch (e) {
      AppLogger.ffi('  lookup CopyGraphDebugSnapshot FAILED: $e');
      rethrow;
    }

    _graphDebugInitialized = true;
    AppLogger.ffi('_initializeGraphDebug: completed successfully');
  }

  List<GraphNodeInfo> _decodeGraphNodes(Pointer<Float> buffer, int actual) {
    final nodes = <GraphNodeInfo>[];
    for (int i = 0; i < actual; i++) {
      final p = buffer + i * 15;
      nodes.add(GraphNodeInfo(
        id: p[0].toInt(),
        bboxCanvas: Rect.fromLTWH(p[1], p[2], p[3], p[4]),
        centroid: Offset(p[5], p[6]),
        area: p[7],
        absenceCount: p[8].toInt(),
        lastSeenFrame: p[9].toInt(),
        createdFrame: p[10].toInt(),
        neighborCount: p[11].toInt(),
        canvasOrigin: Offset(p[12], p[13]),
        matchDistance: p[14].toInt(),
      ));
    }
    return nodes;
  }

  List<GraphNodeInfo> _readGraphNodes({
    required int count,
    required int Function(Pointer<Float>, int) reader,
    required String logLabel,
  }) {
    if (count <= 0) return [];

    final buffer = malloc.allocate<Float>(count * 15 * 4);
    try {
      final actual = reader(buffer, count);
      AppLogger.graphDebug(
        '$logLabel: reader returned $actual nodes (requested $count)',
      );
      return _decodeGraphNodes(buffer, actual);
    } finally {
      malloc.free(buffer);
    }
  }

  Rect? _readGraphBounds({
    required bool Function(Pointer<Int32>) reader,
    required String logLabel,
  }) {
    final buffer = malloc.allocate<Int32>(4 * 4);
    try {
      final ok = reader(buffer);
      AppLogger.graphDebug('$logLabel: bounds ok=$ok');
      if (!ok) return null;
      final minX = buffer[0].toDouble();
      final minY = buffer[1].toDouble();
      final maxX = buffer[2].toDouble();
      final maxY = buffer[3].toDouble();
      AppLogger.graphDebug('$logLabel: ($minX, $minY) -> ($maxX, $maxY)');
      return Rect.fromLTRB(minX, minY, maxX, maxY);
    } finally {
      malloc.free(buffer);
    }
  }

  Map<int, List<Offset>> _readGraphContours({
    required int Function(Pointer<Float>, int)? reader,
    required String logLabel,
  }) {
    if (reader == null) return {};

    const maxFloats = 500000;
    final buffer = malloc.allocate<Float>(maxFloats * 4);
    try {
      final written = reader(buffer, maxFloats);
      AppLogger.graphDebug('$logLabel: reader wrote $written floats');
      final result = <int, List<Offset>>{};
      int i = 0;
      while (i + 1 < written) {
        final nodeId = buffer[i].toInt();
        final numPoints = buffer[i + 1].toInt();
        i += 2;
        final points = <Offset>[];
        for (int j = 0; j < numPoints && i + 1 < written; j++) {
          points.add(Offset(buffer[i], buffer[i + 1]));
          i += 2;
        }
        result[nodeId] = points;
      }
      return result;
    } finally {
      malloc.free(buffer);
    }
  }

  int getGraphNodeCount() {
    _initializeGraphDebug();
    final count = _getGraphNodeCount();
    AppLogger.graphDebug('getGraphNodeCount() returned $count');
    return count;
  }

  List<GraphNodeInfo> getGraphNodes() {
    _initializeGraphDebug();
    final count = _getGraphNodeCount();
    AppLogger.graphDebug('getGraphNodes: _getGraphNodeCount=$count');
    return _readGraphNodes(
      count: count,
      reader: (buffer, maxNodes) => _getGraphNodes(buffer, maxNodes),
      logLabel: 'getGraphNodes',
    );
  }

  List<int> getNodeNeighbors(int nodeId) {
    _initializeGraphDebug();
    const maxNeighbors = 32;
    final buffer = malloc.allocate<Int32>(maxNeighbors * 4);
    try {
      final count = _getGraphNodeNeighbors(nodeId, buffer, maxNeighbors);
      return [for (int i = 0; i < count; i++) buffer[i]];
    } finally {
      malloc.free(buffer);
    }
  }

  NodeComparison? compareNodes(int idA, int idB) {
    _initializeGraphDebug();
    final buffer = malloc.allocate<Float>(5 * 4);
    try {
      final ok = _compareGraphNodes(idA, idB, buffer);
      AppLogger.graphDebug(
          'compareNodes($idA, $idB): ok=$ok');
      if (!ok) return null;
      return NodeComparison(
        shapeDistance: buffer[0],
        centroidDistance: buffer[1],
        bboxIntersectionArea: buffer[2],
        andOverlapPixels: buffer[3],
        maskOverlapRatio: buffer[4],
      );
    } finally {
      malloc.free(buffer);
    }
  }

  bool moveNode(int id, double cx, double cy) {
    _initializeGraphDebug();
    return _moveGraphNode(id, cx, cy);
  }

  Rect? getCanvasBounds() {
    _initializeGraphDebug();
    return _readGraphBounds(
      reader: (buffer) => _getGraphCanvasBounds(buffer),
      logLabel: 'getCanvasBounds',
    );
  }

  /// Returns a map of node_id -> list of contour points (in canvas coordinates).
  Map<int, List<Offset>> getGraphNodeContours() {
    _initializeGraphDebug();
    return _readGraphContours(
      reader: _getGraphNodeContours == null
          ? null
          : (buffer, maxFloats) => _getGraphNodeContours!(buffer, maxFloats),
      logLabel: 'getGraphNodeContours',
    );
  }

  bool captureGraphSnapshot(int slot) {
    _initializeGraphDebug();
    final ok = _captureGraphDebugSnapshot(slot);
    AppLogger.graphDebug('captureGraphSnapshot($slot): ok=$ok');
    return ok;
  }

  List<GraphNodeInfo> getGraphSnapshotNodes(int slot) {
    _initializeGraphDebug();
    final count = _getGraphSnapshotNodeCount(slot);
    AppLogger.graphDebug('getGraphSnapshotNodes($slot): count=$count');
    return _readGraphNodes(
      count: count,
      reader: (buffer, maxNodes) => _getGraphSnapshotNodes(slot, buffer, maxNodes),
      logLabel: 'getGraphSnapshotNodes($slot)',
    );
  }

  Rect? getGraphSnapshotCanvasBounds(int slot) {
    _initializeGraphDebug();
    return _readGraphBounds(
      reader: (buffer) => _getGraphSnapshotCanvasBounds(slot, buffer),
      logLabel: 'getGraphSnapshotCanvasBounds($slot)',
    );
  }

  Map<int, List<Offset>> getGraphSnapshotNodeContours(int slot) {
    _initializeGraphDebug();
    return _readGraphContours(
      reader: _getGraphSnapshotNodeContours == null
          ? null
          : (buffer, maxFloats) =>
              _getGraphSnapshotNodeContours!(slot, buffer, maxFloats),
      logLabel: 'getGraphSnapshotNodeContours($slot)',
    );
  }

  GraphSnapshotComparison? compareGraphSnapshotNodes(
    int slotA,
    int idA,
    int slotB,
    int idB,
  ) {
    _initializeGraphDebug();
    final buffer = malloc.allocate<Float>(6 * 4);
    try {
      final ok = _compareGraphSnapshotNodes(slotA, idA, slotB, idB, buffer);
      AppLogger.graphDebug(
        'compareGraphSnapshotNodes($slotA:$idA, $slotB:$idB): ok=$ok',
      );
      if (!ok) return null;
      return GraphSnapshotComparison(
        shapeDistance: buffer[0],
        widthRatio: buffer[1],
        heightRatio: buffer[2],
        longEdgeSimilarity: buffer[3],
        shortEdgeSimilarity: buffer[4],
        averageEdgeSimilarity: buffer[5],
      );
    } finally {
      malloc.free(buffer);
    }
  }

  bool combineGraphSnapshots(
    int slotA,
    int slotB, {
    int? anchorIdA,
    int? anchorIdB,
  }) {
    _initializeGraphDebug();
    final resolvedAnchorIdA =
        anchorIdA != null && anchorIdB != null ? anchorIdA : -1;
    final resolvedAnchorIdB =
        anchorIdA != null && anchorIdB != null ? anchorIdB : -1;
    final ok = _combineGraphDebugSnapshots(
      slotA,
      resolvedAnchorIdA,
      slotB,
      resolvedAnchorIdB,
    );
    AppLogger.graphDebug(
      'combineGraphSnapshots($slotA:$resolvedAnchorIdA, '
      '$slotB:$resolvedAnchorIdB): ok=$ok',
    );
    return ok;
  }

  bool copyGraphSnapshot(int sourceSlot, int targetSlot) {
    _initializeGraphDebug();
    final ok = _copyGraphDebugSnapshot(sourceSlot, targetSlot);
    AppLogger.graphDebug(
      'copyGraphSnapshot($sourceSlot -> $targetSlot): ok=$ok',
    );
    return ok;
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
