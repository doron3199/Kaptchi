import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'package:camera/camera.dart';
import 'package:ffi/ffi.dart';

// Define the C function signature
typedef ProcessFrameC = Void Function(Pointer<Uint8> bytes, Int32 width, Int32 height, Int32 mode);
typedef ProcessFrameDart = void Function(Pointer<Uint8> bytes, int width, int height, int mode);

enum ProcessingMode { none, invert, whiteboard, removeObstacles }

class ImageProcessingService {
  static final ImageProcessingService instance = ImageProcessingService._();
  ImageProcessingService._();

  DynamicLibrary? _nativeLib;
  ProcessFrameDart? _processFrameFunc;
  ProcessingMode _currentMode = ProcessingMode.none;

  Future<void> initialize() async {
    if (Platform.isWindows) {
      try {
        // On Windows, the functions are compiled into the main executable
        _nativeLib = DynamicLibrary.executable();
        
        _processFrameFunc = _nativeLib!
            .lookup<NativeFunction<ProcessFrameC>>('process_frame')
            .asFunction<ProcessFrameDart>();
            
        print('ImageProcessingService: Loaded native C++ functions.');
      } catch (e) {
        print('ImageProcessingService: Failed to load native library: $e');
      }
    }
  }

  void setProcessingMode(ProcessingMode mode) {
    _currentMode = mode;
    print('ImageProcessingService: Mode set to $mode');
  }

  // Helper to attach to track (placeholder for WebRTC)
  void attachToTrack(dynamic track) {
    print('ImageProcessingService: Attaching to track $track');
  }

  Future<Uint8List?> processImageAndEncode(CameraImage image) async {
    // If mode is none, return null so the UI shows the raw camera preview
    // Also check if function is loaded
    if (_currentMode == ProcessingMode.none) return null;
    
    if (_processFrameFunc == null) {
        print('Warning: Native process_frame function not loaded.');
        return null;
    }

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

      // Call C++ function (modifies data in-place)
      _processFrameFunc!(ptr, width, height, _currentMode.index);

      // Copy back to Dart Uint8List (this is expensive, but necessary for display in Image.memory)
      // Ideally, we would render the texture directly in C++, but for now we encode to BMP/RGBA
      
      // Construct a simple BMP header to make it displayable by Image.memory
      // (Raw RGBA bytes cannot be displayed directly by Image.memory)
      return _addBmpHeader(ptr.asTypedList(size), width, height);

    } catch (e) {
      print('Error processing frame: $e');
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
