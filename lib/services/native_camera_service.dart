import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

typedef GetTextureIdFunc = Int64 Function();
typedef GetTextureId = int Function();

typedef StartCameraFunc = Void Function();
typedef StartCamera = void Function();

typedef StopCameraFunc = Void Function();
typedef StopCamera = void Function();

typedef SetCameraFilterFunc = Void Function(Int32 mode);
typedef SetCameraFilter = void Function(int mode);

typedef GetFrameDataFunc = Void Function(Pointer<Uint8> buffer, Int32 size);
typedef GetFrameData = void Function(Pointer<Uint8> buffer, int size);

typedef GetFrameWidthFunc = Int32 Function();
typedef GetFrameWidth = int Function();

typedef GetFrameHeightFunc = Int32 Function();
typedef GetFrameHeight = int Function();

class NativeCameraService {
  static final NativeCameraService _instance = NativeCameraService._internal();
  factory NativeCameraService() => _instance;
  NativeCameraService._internal();

  late DynamicLibrary _nativeLib;
  late GetTextureId _getTextureId;
  late StartCamera _startCamera;
  late StopCamera _stopCamera;
  late SetCameraFilter _setCameraFilter;
  late GetFrameData _getFrameData;
  late GetFrameWidth _getFrameWidth;
  late GetFrameHeight _getFrameHeight;

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
    _stopCamera = _nativeLib
        .lookup<NativeFunction<StopCameraFunc>>('StopCamera')
        .asFunction();
    _setCameraFilter = _nativeLib
        .lookup<NativeFunction<SetCameraFilterFunc>>('SetCameraFilter')
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

  void stop() {
    initialize();
    _stopCamera();
  }

  void setFilter(int mode) {
    initialize();
    _setCameraFilter(mode);
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
}
