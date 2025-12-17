import 'dart:ffi';
import 'dart:io';
import 'dart:ui'; // For Offset
import 'package:camera/camera.dart';
import 'package:flutter/foundation.dart';
import 'package:ffi/ffi.dart';

typedef ProcessFrameSequenceC =
    Void Function(
      Pointer<Uint8> bytes,
      Int32 width,
      Int32 height,
      Pointer<Int32> modes,
      Int32 count,
    );
typedef ProcessFrameSequenceDart =
    void Function(
      Pointer<Uint8> bytes,
      int width,
      int height,
      Pointer<Int32> modes,
      int count,
    );

typedef GetFrameDataFunc = Void Function(Pointer<Uint8>, Int32);
typedef GetFrameData = void Function(Pointer<Uint8>, int);

typedef GetFrameWidthFunc = Int32 Function();
typedef GetFrameWidth = int Function();

typedef GetFrameHeightFunc = Int32 Function();
typedef GetFrameHeight = int Function();

// Perspective Crop Bindings
typedef ProcessPerspectiveCropFunc =
    Void Function(
      Pointer<Uint8> inputBytes,
      Int32 inputSize,
      Pointer<Double> corners,
      Pointer<Pointer<Uint8>> outputBytes,
      Pointer<Int32> outputSize,
    );
typedef ProcessPerspectiveCrop =
    void Function(
      Pointer<Uint8> inputBytes,
      int inputSize,
      Pointer<Double> corners,
      Pointer<Pointer<Uint8>> outputBytes,
      Pointer<Int32> outputSize,
    );

// FreeBuffer
typedef FreeBufferFunc = Void Function(Pointer<Uint8>);
typedef FreeBuffer = void Function(Pointer<Uint8>);

enum ProcessingMode {
  none, // 0
  invert, // 1
  whiteboardLegacy, // 2
  blurLegacy, // 3
  smartWhiteboard, // 4
  smartObstacle, // 5
  movingAverage, // 6
  clahe, // 7
  sharpening, // 8
  ppHumanSeg, // 9
  mediaPipeSelfie, // 10
  yolo11PersonDetection, // 11
  stabilization, // 12
  lightStabilization, // 13
  cornerSmoothing, // 14
}

class ImageProcessingService {
  static final ImageProcessingService instance = ImageProcessingService._();
  ImageProcessingService._();

  DynamicLibrary? _nativeLib;
  ProcessFrameSequenceDart? _processFrameSequenceRgbaFunc;
  ProcessFrameSequenceDart? _processFrameSequenceBgraFunc;
  List<ProcessingMode> _activeModes = [];

  // Crop
  ProcessPerspectiveCrop? _processPerspectiveCrop;
  FreeBuffer? _freeBuffer;

  Future<void> initialize() async {
    if (Platform.isWindows) {
      try {
        // On Windows, the functions are compiled into the main executable
        _nativeLib = DynamicLibrary.executable();

        try {
          _processFrameSequenceRgbaFunc = _nativeLib!
              .lookup<NativeFunction<ProcessFrameSequenceC>>(
                'process_frame_sequence_rgba',
              )
              .asFunction<ProcessFrameSequenceDart>();
        } catch (e) {
          debugPrint(
            'ImageProcessingService: process_frame_sequence_rgba not found (might need rebuild): $e',
          );
        }

        try {
          _processFrameSequenceBgraFunc = _nativeLib!
              .lookup<NativeFunction<ProcessFrameSequenceC>>(
                'process_frame_sequence_bgra',
              )
              .asFunction<ProcessFrameSequenceDart>();
        } catch (e) {
          debugPrint(
            'ImageProcessingService: process_frame_sequence_bgra not found (might need rebuild): $e',
          );
        }

        // Load perspective crop functions
        try {
          _processPerspectiveCrop = _nativeLib!
              .lookup<NativeFunction<ProcessPerspectiveCropFunc>>(
                'ProcessPerspectiveCrop',
              )
              .asFunction<ProcessPerspectiveCrop>();
          debugPrint(
            'ImageProcessingService: ProcessPerspectiveCrop loaded successfully.',
          );
        } catch (e) {
          debugPrint(
            'ImageProcessingService: ProcessPerspectiveCrop not found: $e',
          );
        }

        try {
          _freeBuffer = _nativeLib!
              .lookup<NativeFunction<FreeBufferFunc>>('FreeBuffer')
              .asFunction<FreeBuffer>();
          debugPrint('ImageProcessingService: FreeBuffer loaded successfully.');
        } catch (e) {
          debugPrint('ImageProcessingService: FreeBuffer not found: $e');
        }

        debugPrint('ImageProcessingService: Loaded native C++ functions.');
      } catch (e) {
        debugPrint('ImageProcessingService: Failed to load native library: $e');
      }
    }
  }

  void setProcessingModes(List<ProcessingMode> modes) {
    _activeModes = modes;
    debugPrint('ImageProcessingService: Modes set to $modes');
  }

  Future<Uint8List?> applyPerspectiveCrop(
    Uint8List imageBytes,
    List<Offset> corners,
  ) async {
    if (corners.length != 4) return null;

    // Allocate Input
    final inputPtr = calloc<Uint8>(imageBytes.length);
    final inputList = inputPtr.asTypedList(imageBytes.length);
    inputList.setAll(0, imageBytes);

    // Allocate Corners (x1, y1, x2, y2 ...)
    final cornersPtr = calloc<Double>(8);
    for (int i = 0; i < 4; i++) {
      cornersPtr[i * 2] = corners[i].dx;
      cornersPtr[i * 2 + 1] = corners[i].dy;
    }

    // Allocate Outputs
    final outBytesPtrPtr = calloc<Pointer<Uint8>>();
    final outSizePtr = calloc<Int32>();

    try {
      if (_processPerspectiveCrop == null) {
        debugPrint('ProcessPerspectiveCrop not loaded');
        return null;
      }

      _processPerspectiveCrop!(
        inputPtr,
        imageBytes.length,
        cornersPtr,
        outBytesPtrPtr,
        outSizePtr,
      );

      final int size = outSizePtr.value;
      final Pointer<Uint8> outBytesPtr = outBytesPtrPtr.value;

      if (size > 0 && outBytesPtr != nullptr) {
        final result = Uint8List.fromList(outBytesPtr.asTypedList(size));
        if (_freeBuffer != null) {
          _freeBuffer!(outBytesPtr);
        }
        return result;
      }
      return null;
    } catch (e) {
      debugPrint('Perspective Crop Error: $e');
      return null;
    } finally {
      calloc.free(inputPtr);
      calloc.free(cornersPtr);
      calloc.free(outBytesPtrPtr);
      calloc.free(outSizePtr);
    }
  }

  Future<Uint8List?> processRawRgba(
    Uint8List rgbaData,
    int width,
    int height,
  ) async {
    if (_activeModes.isEmpty) return null;

    final int size = rgbaData.length;
    final Pointer<Uint8> ptr = malloc.allocate<Uint8>(size);

    try {
      final Uint8List ptrList = ptr.asTypedList(size);
      ptrList.setAll(0, rgbaData);

      if (_processFrameSequenceRgbaFunc != null) {
        // Use the optimized sequence function
        final modesPtr = malloc.allocate<Int32>(_activeModes.length * 4);
        try {
          final modesList = modesPtr.asTypedList(_activeModes.length);
          for (int i = 0; i < _activeModes.length; i++) {
            modesList[i] = _activeModes[i].index;
          }
          _processFrameSequenceRgbaFunc!(
            ptr,
            width,
            height,
            modesPtr,
            _activeModes.length,
          );
        } finally {
          malloc.free(modesPtr);
        }
      }

      return _addBmpHeader(ptr.asTypedList(size), width, height);
    } catch (e) {
      debugPrint('Error processing raw frame: $e');
      return null;
    } finally {
      malloc.free(ptr);
    }
  }

  Future<Uint8List?> processImageAndEncode(CameraImage image) async {
    // If no modes active, return null so the UI shows the raw camera preview
    if (_activeModes.isEmpty) return null;

    // IMPORTANT: This assumes BGRA format (standard on Windows)
    // We need to copy the plane data to a heap pointer to pass to C++
    final int width = image.width;
    final int height = image.height;
    final int size = width * height * 4; // 4 bytes per pixel (BGRA)

    // Allocate memory on the C++ heap
    final Pointer<Uint8> ptr = malloc.allocate<Uint8>(size);

    try {
      // Copy camera data to pointer
      // Note: CameraImage on Windows usually has one plane for BGRA
      final Uint8List bytes = image.planes[0].bytes;
      // We can use setRange for faster copy if the list is viewable
      // But for safety with FFI, a loop or asTypedList copy is standard
      final Uint8List ptrList = ptr.asTypedList(size);
      ptrList.setAll(0, bytes);

      // Call C++ function for each active mode in sequence
      if (_processFrameSequenceBgraFunc != null) {
        final modesPtr = malloc.allocate<Int32>(_activeModes.length * 4);
        try {
          final modesList = modesPtr.asTypedList(_activeModes.length);
          for (int i = 0; i < _activeModes.length; i++) {
            modesList[i] = _activeModes[i].index;
          }
          _processFrameSequenceBgraFunc!(
            ptr,
            width,
            height,
            modesPtr,
            _activeModes.length,
          );
        } finally {
          malloc.free(modesPtr);
        }
      }

      // Copy back to Dart Uint8List (this is expensive, but necessary for display in Image.memory)
      // Ideally, we would render the texture directly in C++, but for now we encode to BMP/RGBA

      // Construct a simple BMP header to make it displayable by Image.memory
      // (Raw RGBA bytes cannot be displayed directly by Image.memory)
      return _addBmpHeader(ptr.asTypedList(size), width, height);
    } catch (e) {
      debugPrint('Error processing frame: $e');
      return null;
    } finally {
      malloc.free(ptr);
    }
  }

  // Helper to wrap raw BGRA bytes in a BMP header so Flutter can display it
  Uint8List _addBmpHeader(Uint8List rgbaData, int width, int height) {
    final int contentSize = rgbaData.length;
    final int fileSize = 54 + contentSize;
    final Uint8List bmp = Uint8List(fileSize);
    final ByteData bd = bmp.buffer.asByteData();

    // Bitmap Header
    bd.setUint8(0, 0x42); // 'B'
    bd.setUint8(1, 0x4D); // 'M'
    bd.setUint32(2, fileSize, Endian.little);
    bd.setUint32(10, 54, Endian.little); // Offset to pixel data

    // DIB Header
    bd.setUint32(14, 40, Endian.little); // Header size
    bd.setInt32(18, width, Endian.little);
    bd.setInt32(22, -height, Endian.little); // Negative height for top-down
    bd.setUint16(26, 1, Endian.little); // Planes
    bd.setUint16(28, 32, Endian.little); // BPP (32 for BGRA)
    bd.setUint32(30, 0, Endian.little); // Compression (BI_RGB)
    bd.setUint32(34, contentSize, Endian.little);

    // Copy pixel data
    bmp.setRange(54, fileSize, rgbaData);
    return bmp;
  }
}
