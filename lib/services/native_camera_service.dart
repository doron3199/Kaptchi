import 'dart:ffi' hide Size;
import 'dart:io';
import 'dart:math' as math;
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

typedef SetDuplicateDebugModeFunc = Void Function(Bool enabled);
typedef SetDuplicateDebugMode = void Function(bool enabled);

typedef GetDuplicateDebugModeFunc = Bool Function();
typedef GetDuplicateDebugMode = bool Function();

typedef SetCanvasEnhanceThresholdFunc = Void Function(Float threshold);
typedef SetCanvasEnhanceThreshold = void Function(double threshold);

typedef SetAbsenceScoreSeenThresholdFunc = Void Function(Float threshold);
typedef SetAbsenceScoreSeenThreshold = void Function(double threshold);

typedef GetAbsenceScoreSeenThresholdFunc = Float Function();
typedef GetAbsenceScoreSeenThreshold = double Function();

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

typedef GetGraphHardEdgesFunc = Int32 Function(Pointer<Int32> buffer, Int32 maxEdges);
typedef GetGraphHardEdgesFFI = int Function(Pointer<Int32> buffer, int maxEdges);

typedef GetGraphNodeNeighborsFunc = Int32 Function(
  Int32 nodeId, Pointer<Int32> neighbors, Int32 maxNeighbors);
typedef GetGraphNodeNeighborsFFI = int Function(
  int nodeId, Pointer<Int32> neighbors, int maxNeighbors);

typedef CompareGraphNodesFunc = Bool Function(
  Int32 idA, Int32 idB, Pointer<Float> result);
typedef CompareGraphNodesFFI = bool Function(
  int idA, int idB, Pointer<Float> result);

typedef CompareGraphNodesAtOffsetFunc = Bool Function(
  Int32 idA, Int32 idB, Float dx, Float dy, Pointer<Float> result);
typedef CompareGraphNodesAtOffsetFFI = bool Function(
  int idA, int idB, double dx, double dy, Pointer<Float> result);

typedef MoveGraphNodeFunc = Bool Function(
  Int32 nodeId, Float newCx, Float newCy);
typedef MoveGraphNodeFFI = bool Function(
  int nodeId, double newCx, double newCy);

typedef DeleteGraphNodeFunc = Bool Function(Int32 nodeId);
typedef DeleteGraphNodeFFI = bool Function(int nodeId);

typedef ApplyUserEditsFunc = Bool Function(
  Pointer<Int32> deleteIds, Int32 deleteCount,
  Pointer<Float> moves, Int32 moveCount);
typedef ApplyUserEditsFFI = bool Function(
  Pointer<Int32> deleteIds, int deleteCount,
  Pointer<Float> moves, int moveCount);

typedef LockAllGraphNodesFunc = Int32 Function();
typedef LockAllGraphNodesFFI = int Function();

typedef GetGraphCanvasBoundsFunc = Bool Function(Pointer<Int32> bounds);
typedef GetGraphCanvasBoundsFFI = bool Function(Pointer<Int32> bounds);

typedef GetGraphNodeContoursFunc = Int32 Function(Pointer<Float> buffer, Int32 maxFloats);
typedef GetGraphNodeContoursFFI = int Function(Pointer<Float> buffer, int maxFloats);

typedef GetGraphNodeMasksFunc = Int32 Function(Pointer<Uint8> buffer, Int32 maxBytes);
typedef GetGraphNodeMasksFFI = int Function(Pointer<Uint8> buffer, int maxBytes);

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

// Canvas full-res export FFI types
typedef GetCanvasFullResRgbaFunc = Bool Function(
  Pointer<Uint8> buffer,
  Int32 maxW,
  Int32 maxH,
  Pointer<Int32> outW,
  Pointer<Int32> outH,
);
typedef GetCanvasFullResRgbaDart = bool Function(
  Pointer<Uint8> buffer,
  int maxW,
  int maxH,
  Pointer<Int32> outW,
  Pointer<Int32> outH,
);

typedef GetCanvasVersionFunc = Uint64 Function();
typedef GetCanvasVersionDart = int Function();

typedef GetCanvasOverviewJpegFunc = Bool Function(
    Pointer<Uint8> buffer, Int32 maxBytes, Pointer<Int32> outSize, Int32 quality, Int32 maxDim);
typedef GetCanvasOverviewJpegDart = bool Function(
    Pointer<Uint8> buffer, int maxBytes, Pointer<Int32> outSize, int quality, int maxDim);

typedef GetDisplayFrameIdFunc = Uint64 Function();
typedef GetDisplayFrameIdDart = int Function();

typedef GetFrameDataJpegFunc = Bool Function(Pointer<Uint8> buffer, Int32 maxBytes, Pointer<Int32> outSize, Int32 quality);
typedef GetFrameDataJpegDart = bool Function(Pointer<Uint8> buffer, int maxBytes, Pointer<Int32> outSize, int quality);

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
  late GetDisplayFrameIdDart _getDisplayFrameId;
  late GetFrameDataJpegDart _getFrameDataJpeg;
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
  late SetDuplicateDebugMode _setDuplicateDebugMode;
  late GetDuplicateDebugMode _getDuplicateDebugMode;
  late SetCanvasEnhanceThreshold _setCanvasEnhanceThreshold;
  late SetAbsenceScoreSeenThreshold _setAbsenceScoreSeenThreshold;
  late GetAbsenceScoreSeenThreshold _getAbsenceScoreSeenThreshold;
  
  late GetCanvasVersionDart _getCanvasVersion;
  late GetCanvasOverviewJpegDart _getCanvasOverviewJpeg;
  
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
        
    try {
        _getDisplayFrameId = _nativeLib
            .lookup<NativeFunction<GetDisplayFrameIdFunc>>('GetDisplayFrameId')
            .asFunction();
            
        _getFrameDataJpeg = _nativeLib
            .lookup<NativeFunction<GetFrameDataJpegFunc>>('GetFrameDataJpeg')
            .asFunction();
    } catch (_) {}
    
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
    _setDuplicateDebugMode = _nativeLib
        .lookup<NativeFunction<SetDuplicateDebugModeFunc>>(
          'SetDuplicateDebugMode',
        )
        .asFunction();
    _getDuplicateDebugMode = _nativeLib
        .lookup<NativeFunction<GetDuplicateDebugModeFunc>>(
          'GetDuplicateDebugMode',
        )
        .asFunction();
    _setCanvasEnhanceThreshold = _nativeLib
        .lookup<NativeFunction<SetCanvasEnhanceThresholdFunc>>(
          'SetCanvasEnhanceThreshold',
        )
        .asFunction();
    _setAbsenceScoreSeenThreshold = _nativeLib
        .lookup<NativeFunction<SetAbsenceScoreSeenThresholdFunc>>(
          'SetAbsenceScoreSeenThreshold',
        )
        .asFunction();
    _getAbsenceScoreSeenThreshold = _nativeLib
        .lookup<NativeFunction<GetAbsenceScoreSeenThresholdFunc>>(
          'GetAbsenceScoreSeenThreshold',
        )
        .asFunction();

    _getCanvasVersion = _nativeLib
        .lookup<NativeFunction<GetCanvasVersionFunc>>('GetCanvasVersion')
        .asFunction();
        
    _getCanvasOverviewJpeg = _nativeLib
        .lookup<NativeFunction<GetCanvasOverviewJpegFunc>>('GetCanvasOverviewJpeg')
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

  int getDisplayFrameId() {
    initialize();
    return _getDisplayFrameId();
  }

  Uint8List? getFrameDataJpeg({int quality = 80}) {
    initialize();
    
    final maxBytes = 4 * 1024 * 1024;
    final buffer = malloc.allocate<Uint8>(maxBytes);
    final outSize = malloc.allocate<Int32>(4);
    
    try {
      final success = _getFrameDataJpeg(buffer, maxBytes, outSize, quality);
      if (!success || outSize.value <= 0) return null;
      
      final byteList = buffer.asTypedList(outSize.value);
      return Uint8List.fromList(byteList);
    } finally {
      malloc.free(buffer);
      malloc.free(outSize);
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

  /// Returns the current version tick of the canvas, used for polling changes.
  int getCanvasVersion() {
    initialize();
    return _getCanvasVersion();
  }

  /// Returns a JPEG compressed overview of the full canvas, or null if unavailable.
  Uint8List? getCanvasOverviewJpeg(int maxDim, {int quality = 80}) {
    initialize();
    if (maxDim <= 0) return null;
    
    // Allocate 4MB, plenty for a high res JPEG
    final maxBytes = 4 * 1024 * 1024;
    final buffer = malloc.allocate<Uint8>(maxBytes);
    final outSize = malloc.allocate<Int32>(4);
    
    try {
      final success = _getCanvasOverviewJpeg(buffer, maxBytes, outSize, quality, maxDim);
      if (!success || outSize.value <= 0) return null;
      
      final byteList = buffer.asTypedList(outSize.value);
      return Uint8List.fromList(byteList);
    } finally {
      malloc.free(buffer);
      malloc.free(outSize);
    }
  }

  /// Toggle OpenCV debug popup windows for the whiteboard pipeline
  void setWhiteboardDebug(bool enabled) {
    initialize();
    _setWhiteboardDebug(enabled);
  }

  void setDuplicateDebugMode(bool enabled) {
    initialize();
    _setDuplicateDebugMode(enabled);
  }

  bool getDuplicateDebugMode() {
    initialize();
    return _getDuplicateDebugMode();
  }

  /// Set the DoG noise-suppression threshold for WhiteboardEnhance (1–30).
  void setCanvasEnhanceThreshold(double threshold) {
    initialize();
    _setCanvasEnhanceThreshold(threshold);
  }

  void setAbsenceScoreSeenThreshold(double threshold) {
    initialize();
    _setAbsenceScoreSeenThreshold(threshold);
  }

  double getAbsenceScoreSeenThreshold() {
    initialize();
    return _getAbsenceScoreSeenThreshold();
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

  // --- Virtual Display Manager Methods ---

  late IsVirtualDisplayInstalled _isVirtualDisplayInstalled;
  late CreateVirtualDisplay _createVirtualDisplay;
  late RemoveVirtualDisplayDart _removeVirtualDisplay;
  late GetVirtualDisplayIndex _getVirtualDisplayIndex;
  late StartVirtualDisplayCapture _startVirtualDisplayCapture;
  late InstallBundledVddDart _installBundledVdd;
  late UninstallVddDriverDart _uninstallVddDriver;
  late SendClickToVirtualDisplay _sendClickToVirtualDisplay;
  late SendScrollToVirtualDisplay _sendScrollToVirtualDisplay;
  late MapDisplayToOriginalDart _mapDisplayToOriginal;

  bool _vddInitialized = false;

  void _initializeVdd() {
    if (_vddInitialized) return;
    initialize();

    _isVirtualDisplayInstalled = _nativeLib
        .lookup<NativeFunction<IsVirtualDisplayInstalledFunc>>(
          'IsVirtualDisplayInstalled',
        )
        .asFunction();
    _createVirtualDisplay = _nativeLib
        .lookup<NativeFunction<CreateVirtualDisplayFunc>>('CreateVirtualDisplay')
        .asFunction();
    _removeVirtualDisplay = _nativeLib
        .lookup<NativeFunction<RemoveVirtualDisplayFunc>>(
          'RemoveVirtualDisplay',
        )
        .asFunction();
    _getVirtualDisplayIndex = _nativeLib
        .lookup<NativeFunction<GetVirtualDisplayIndexFunc>>(
          'GetVirtualDisplayIndex',
        )
        .asFunction();
    _startVirtualDisplayCapture = _nativeLib
        .lookup<NativeFunction<StartVirtualDisplayCaptureFunc>>(
          'StartVirtualDisplayCapture',
        )
        .asFunction();
    _installBundledVdd = _nativeLib
        .lookup<NativeFunction<InstallBundledVddFunc>>('InstallBundledVdd')
        .asFunction();
    _uninstallVddDriver = _nativeLib
        .lookup<NativeFunction<UninstallVddDriverNativeFunc>>('UninstallVddDriver')
        .asFunction();
    _sendClickToVirtualDisplay = _nativeLib
        .lookup<NativeFunction<SendClickToVirtualDisplayFunc>>(
          'SendClickToVirtualDisplay',
        )
        .asFunction();
    _sendScrollToVirtualDisplay = _nativeLib
        .lookup<NativeFunction<SendScrollToVirtualDisplayFunc>>(
          'SendScrollToVirtualDisplay',
        )
        .asFunction();
    _mapDisplayToOriginal = _nativeLib
        .lookup<NativeFunction<MapDisplayToOriginalFunc>>(
          'MapDisplayToOriginal',
        )
        .asFunction();

    _vddInitialized = true;
  }

  /// Check if the Virtual Display Driver is installed
  bool isVirtualDisplayInstalled() {
    _initializeVdd();
    return _isVirtualDisplayInstalled() == 1;
  }

  /// Create a virtual display with the given resolution.
  /// Returns the monitor index, or -1 on failure.
  int createVirtualDisplay(int width, int height) {
    _initializeVdd();
    return _createVirtualDisplay(width, height);
  }

  /// Remove the virtual display
  void removeVirtualDisplay() {
    _initializeVdd();
    _removeVirtualDisplay();
  }

  /// Get the monitor index of the virtual display (-1 if none)
  int getVirtualDisplayIndex() {
    _initializeVdd();
    return _getVirtualDisplayIndex();
  }

  /// Start capturing a window via virtual display (create VD, move window, capture).
  /// Returns true on success.
  bool startVirtualDisplayCapture(int windowHandle) {
    _initializeVdd();
    return _startVirtualDisplayCapture(windowHandle) == 1;
  }

  /// Install the bundled VDD driver from the app's vdd_driver/ directory
  bool installBundledVdd() {
    _initializeVdd();
    return _installBundledVdd() == 1;
  }

  /// Send a mouse click to the virtual display at normalized coordinates.
  /// normalizedX/Y: 0.0-1.0 position within the display.
  /// clickType: 0=left click, 1=right click, 2=left down, 3=left up
  bool sendClickToVirtualDisplay(
    double normalizedX,
    double normalizedY, {
    int clickType = 0,
  }) {
    _initializeVdd();
    return _sendClickToVirtualDisplay(normalizedX, normalizedY, clickType) == 1;
  }

  /// Send a mouse scroll to the virtual display at normalized coordinates.
  /// deltaY: positive = scroll up, negative = scroll down (120 = one notch)
  bool sendScrollToVirtualDisplay(
    double normalizedX,
    double normalizedY,
    int deltaY,
  ) {
    _initializeVdd();
    return _sendScrollToVirtualDisplay(normalizedX, normalizedY, deltaY) == 1;
  }

  /// Map a normalized display coordinate back to the original frame coordinate,
  /// accounting for perspective crop. Returns the mapped (x, y) pair.
  ({double x, double y}) mapDisplayToOriginal(double normalizedX, double normalizedY) {
    _initializeVdd();
    final outX = malloc<Float>(1);
    final outY = malloc<Float>(1);
    try {
      _mapDisplayToOriginal(normalizedX.toDouble(), normalizedY.toDouble(), outX, outY);
      return (x: outX.value.toDouble(), y: outY.value.toDouble());
    } finally {
      malloc.free(outX);
      malloc.free(outY);
    }
  }

  /// Uninstall the VDD driver using pnputil (triggers UAC prompt)
  bool uninstallVddDriver() {
    _initializeVdd();
    return _uninstallVddDriver() == 1;
  }

  // --- Canvas Full-Res Export Methods ---

  GetCanvasFullResRgbaDart? _getCanvasFullResRgba;
  bool _canvasExportInitialized = false;

  void _initializeCanvasExport() {
    if (_canvasExportInitialized) return;
    initialize();

    try {
      _getCanvasFullResRgba = _nativeLib
          .lookup<NativeFunction<GetCanvasFullResRgbaFunc>>(
            'GetCanvasFullResRgba',
          )
          .asFunction();
    } catch (e) {
      _getCanvasFullResRgba = null;
    }

    _canvasExportInitialized = true;
  }

  /// Returns full-resolution RGBA bytes of the canvas, or null if unavailable.
  ({Uint8List bytes, int width, int height})? getCanvasFullResRgba({
    int maxWidth = 4096,
    int maxHeight = 4096,
  }) {
    _initializeCanvasExport();
    if (_getCanvasFullResRgba == null) return null;

    final bufferSize = maxWidth * maxHeight * 4;
    final buffer = malloc.allocate<Uint8>(bufferSize);
    final outW = malloc.allocate<Int32>(4);
    final outH = malloc.allocate<Int32>(4);
    try {
      final ok = _getCanvasFullResRgba!(buffer, maxWidth, maxHeight, outW, outH);
      if (!ok) return null;
      final w = outW.value;
      final h = outH.value;
      if (w <= 0 || h <= 0) return null;
      final bytes = Uint8List.fromList(buffer.asTypedList(w * h * 4));
      return (bytes: bytes, width: w, height: h);
    } finally {
      malloc.free(buffer);
      malloc.free(outW);
      malloc.free(outH);
    }
  }

  // --- Graph Debug Methods ---

  late GetGraphNodeCount _getGraphNodeCount;
  late GetGraphNodesFFI _getGraphNodes;
  late GetGraphHardEdgesFFI _getGraphHardEdges;
  late GetGraphNodeNeighborsFFI _getGraphNodeNeighbors;
  late CompareGraphNodesFFI _compareGraphNodes;
  CompareGraphNodesAtOffsetFFI? _compareGraphNodesAtOffset;
  late MoveGraphNodeFFI _moveGraphNode;
  late DeleteGraphNodeFFI _deleteGraphNode;
  late ApplyUserEditsFFI _applyUserEdits;
  late LockAllGraphNodesFFI _lockAllGraphNodes;
  late GetGraphCanvasBoundsFFI _getGraphCanvasBounds;
  GetGraphNodeContoursFFI? _getGraphNodeContours;
  GetGraphNodeMasksFFI? _getGraphNodeMasks;
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
      _getGraphHardEdges = _nativeLib
          .lookup<NativeFunction<GetGraphHardEdgesFunc>>('GetGraphHardEdges')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphHardEdges: OK');
    } catch (e) {
      AppLogger.ffi('  lookup GetGraphHardEdges FAILED: $e');
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
      _compareGraphNodesAtOffset = _nativeLib
          .lookup<NativeFunction<CompareGraphNodesAtOffsetFunc>>('CompareGraphNodesAtOffset')
          .asFunction();
      AppLogger.ffi('  lookup CompareGraphNodesAtOffset: OK');
    } catch (e) {
      AppLogger.ffi('  lookup CompareGraphNodesAtOffset FAILED (optional): $e');
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
      _deleteGraphNode = _nativeLib
          .lookup<NativeFunction<DeleteGraphNodeFunc>>('DeleteGraphNode')
          .asFunction();
      AppLogger.ffi('  lookup DeleteGraphNode: OK');
    } catch (e) {
      AppLogger.ffi('  lookup DeleteGraphNode FAILED: $e');
      rethrow;
    }
    try {
      _applyUserEdits = _nativeLib
          .lookup<NativeFunction<ApplyUserEditsFunc>>('ApplyUserEdits')
          .asFunction();
      AppLogger.ffi('  lookup ApplyUserEdits: OK');
    } catch (e) {
      AppLogger.ffi('  lookup ApplyUserEdits FAILED: $e');
      rethrow;
    }
    try {
      _lockAllGraphNodes = _nativeLib
          .lookup<NativeFunction<LockAllGraphNodesFunc>>('LockAllGraphNodes')
          .asFunction();
      AppLogger.ffi('  lookup LockAllGraphNodes: OK');
    } catch (e) {
      AppLogger.ffi('  lookup LockAllGraphNodes FAILED: $e');
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
      _getGraphNodeMasks = _nativeLib
          .lookup<NativeFunction<GetGraphNodeMasksFunc>>('GetGraphNodeMasks')
          .asFunction();
      AppLogger.ffi('  lookup GetGraphNodeMasks: OK');
    } catch (e) {
      _getGraphNodeMasks = null;
      AppLogger.ffi('  lookup GetGraphNodeMasks: not found (optional) - $e');
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
      final p = buffer + i * 24;
      nodes.add(GraphNodeInfo(
        id: p[0].toInt(),
        bboxCanvas: Rect.fromLTWH(p[1], p[2], p[3], p[4]),
        centroid: Offset(p[5], p[6]),
        area: p[7],
        absenceScore: p[8].toDouble(),
        lastSeenFrame: p[9].toInt(),
        createdFrame: p[10].toInt(),
        neighborCount: p[11].toInt(),
        canvasOrigin: Offset(p[12], p[13]),
        matchDistance: p[14].toInt(),
        isUserLocked: p[15] > 0.5,
        isDuplicateDebug: p[16] > 0.5,
        duplicatePartnerId: p[17].toInt(),
        duplicatePositionalOverlap: p[18],
        duplicateCentroidIou: p[19],
        duplicateBboxIou: p[20],
        duplicateShapeDifference: p[21],
        duplicateReasonMask: p[22].toInt(),
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

    final buffer = malloc.allocate<Float>(count * 24 * 4);
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

  List<GraphHardEdge> getGraphHardEdges() {
    _initializeGraphDebug();
    final nodeCount = _getGraphNodeCount();
    final maxEdges = math.max(16, (nodeCount * math.max(0, nodeCount - 1)) ~/ 2);
    final buffer = malloc.allocate<Int32>(maxEdges * 2 * 4);
    try {
      final count = _getGraphHardEdges(buffer, maxEdges);
      final edges = <GraphHardEdge>[];
      for (int i = 0; i < count; i++) {
        final offset = i * 2;
        edges.add(
          GraphHardEdge(
            firstNodeId: buffer[offset],
            secondNodeId: buffer[offset + 1],
          ),
        );
      }
      return edges;
    } finally {
      malloc.free(buffer);
    }
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
    final buffer = malloc.allocate<Float>(19 * 4);
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
        bboxIou: buffer[5],
        widthRatio: buffer[6],
        heightRatio: buffer[7],
        centroidAlignedOverlapPixels: buffer[8],
        centroidAlignedOverlapRatio: buffer[9],
        centroidAlignedIou: buffer[10],
        contourRawDistance: buffer[14],
        contourDifference: buffer[15],
        usedShapeContext: buffer[16] > 0.5,
        huRawDistance: buffer[17],
        huDifference: buffer[18],
        sameCreationFrame: buffer[11] > 0.5,
        isDuplicate: buffer[12] > 0.5,
        duplicateReasonMask: buffer[13].toInt(),
      );
    } finally {
      malloc.free(buffer);
    }
  }

  NodeOverlapAtOffset? compareNodesAtOffset(int idA, int idB, double dx, double dy) {
    _initializeGraphDebug();
    if (_compareGraphNodesAtOffset == null) return null;
    final buffer = malloc.allocate<Float>(3 * 4);
    try {
      final ok = _compareGraphNodesAtOffset!(idA, idB, dx, dy, buffer);
      if (!ok) return null;
      return NodeOverlapAtOffset(
        andOverlapPixels: buffer[0],
        maskOverlapRatio: buffer[1],
        bboxIou: buffer[2],
      );
    } finally {
      malloc.free(buffer);
    }
  }

  bool moveNode(int id, double cx, double cy) {
    _initializeGraphDebug();
    return _moveGraphNode(id, cx, cy);
  }

  bool deleteNode(int id) {
    _initializeGraphDebug();
    return _deleteGraphNode(id);
  }

  int lockAllGraphNodes() {
    _initializeGraphDebug();
    return _lockAllGraphNodes();
  }

  /// Atomically apply a batch of user edits (deletes + moves) to the C++ graph.
  /// [deleteIds] — node IDs to delete.
  /// [moves] — list of (nodeId, newCx, newCy) triples for moved nodes.
  bool applyUserEdits({
    required List<int> deleteIds,
    required List<({int id, double cx, double cy})> moves,
  }) {
    _initializeGraphDebug();

    final deleteBuffer = malloc.allocate<Int32>(
        deleteIds.isEmpty ? 4 : deleteIds.length * 4);
    final moveBuffer = malloc.allocate<Float>(
        moves.isEmpty ? 4 : moves.length * 3 * 4);
    try {
      for (int i = 0; i < deleteIds.length; i++) {
        deleteBuffer[i] = deleteIds[i];
      }
      for (int i = 0; i < moves.length; i++) {
        moveBuffer[i * 3 + 0] = moves[i].id.toDouble();
        moveBuffer[i * 3 + 1] = moves[i].cx;
        moveBuffer[i * 3 + 2] = moves[i].cy;
      }
      return _applyUserEdits(
          deleteBuffer, deleteIds.length, moveBuffer, moves.length);
    } finally {
      malloc.free(deleteBuffer);
      malloc.free(moveBuffer);
    }
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

  /// Returns per-node RGBA mask images: {nodeId: NodeMaskImage(width, height, rgbaBytes)}.
  /// Color pixels on white transparent background.
  Map<int, NodeMaskImage> getGraphNodeMasks() {
    _initializeGraphDebug();
    if (_getGraphNodeMasks == null) return {};

    // Allocate generous buffer: 200 MB to ensure all nodes get masks
    const maxBytes = 200 * 1024 * 1024;
    final buffer = malloc.allocate<Uint8>(maxBytes);
    try {
      final written = _getGraphNodeMasks!(buffer, maxBytes);
      if (written <= 0) return {};

      final result = <int, NodeMaskImage>{};
      int offset = 0;
      while (offset + 12 <= written) {
        // Read header: node_id(4), width(4), height(4)
        final header = buffer.cast<Int32>() + (offset ~/ 4);
        final idBytes = header.value;
        final wBytes = (header + 1).value;
        final hBytes = (header + 2).value;
        offset += 12;

        final pixelBytes = wBytes * hBytes * 4;
        if (offset + pixelBytes > written) break;
        if (wBytes <= 0 || hBytes <= 0) break;

        // Copy RGBA bytes
        final rgba = Uint8List(pixelBytes);
        for (int i = 0; i < pixelBytes; i++) {
          rgba[i] = (buffer + offset + i).value;
        }
        offset += pixelBytes;

        result[idBytes] = NodeMaskImage(
          width: wBytes,
          height: hBytes,
          rgbaBytes: rgba,
        );
      }

      AppLogger.graphDebug('getGraphNodeMasks: ${result.length} masks, $written bytes');
      return result;
    } finally {
      malloc.free(buffer);
    }
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

// FFI type definitions for Virtual Display Manager
typedef IsVirtualDisplayInstalledFunc = Int32 Function();
typedef IsVirtualDisplayInstalled = int Function();

typedef CreateVirtualDisplayFunc = Int32 Function(Int32 width, Int32 height);
typedef CreateVirtualDisplay = int Function(int width, int height);

typedef RemoveVirtualDisplayFunc = Void Function();
typedef RemoveVirtualDisplayDart = void Function();

typedef GetVirtualDisplayIndexFunc = Int32 Function();
typedef GetVirtualDisplayIndex = int Function();

typedef StartVirtualDisplayCaptureFunc = Int32 Function(Int64 windowHandle);
typedef StartVirtualDisplayCapture = int Function(int windowHandle);

typedef InstallBundledVddFunc = Int32 Function();
typedef InstallBundledVddDart = int Function();

typedef UninstallVddDriverNativeFunc = Int32 Function();
typedef UninstallVddDriverDart = int Function();

typedef SendClickToVirtualDisplayFunc =
    Int32 Function(Float normalizedX, Float normalizedY, Int32 clickType);
typedef SendClickToVirtualDisplay =
    int Function(double normalizedX, double normalizedY, int clickType);

typedef SendScrollToVirtualDisplayFunc =
    Int32 Function(Float normalizedX, Float normalizedY, Int32 deltaY);
typedef SendScrollToVirtualDisplay =
    int Function(double normalizedX, double normalizedY, int deltaY);

typedef MapDisplayToOriginalFunc =
    Int32 Function(Float normalizedX, Float normalizedY, Pointer<Float> outX, Pointer<Float> outY);
typedef MapDisplayToOriginalDart =
    int Function(double normalizedX, double normalizedY, Pointer<Float> outX, Pointer<Float> outY);
