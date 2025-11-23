import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';
import 'package:image/image.dart' as img;
import 'package:file_picker/file_picker.dart';
import '../services/image_processing_service.dart';
import '../widgets/native_camera_view.dart';
import '../services/native_camera_service.dart';

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> {
  final _ipController = TextEditingController();
  final _transformationController = TransformationController();
  
  CameraController? _controller;
  List<CameraDescription> _cameras = [];
  int _selectedCameraIndex = -1;
  bool _isHighQuality = false;
  bool _isInitialized = false;
  
  // Image processing state
  final ValueNotifier<Uint8List?> _processedImageNotifier = ValueNotifier(null);
  bool _isProcessingFrame = false;
  ProcessingMode _processingMode = ProcessingMode.none;
  
  // Captured images for PDF export
  final List<({Uint8List bytes, int width, int height})> _capturedImages = [];

  // Sidebar state
  bool _isSidebarOpen = false;
  final TextEditingController _pdfNameController = TextEditingController();
  final TextEditingController _pdfPathController = TextEditingController();

  @override
  void initState() {
    super.initState();
    _init();
    _setDefaultPath();
  }

  Future<void> _setDefaultPath() async {
    final dir = await getApplicationDocumentsDirectory();
    setState(() {
      _pdfPathController.text = dir.path;
    });
  }

  Future<void> _init() async {
    // If we are on Windows, we use the native C++ implementation
    if (Platform.isWindows) {
      setState(() {
        _isInitialized = true;
      });
      return;
    }
    
    await _loadCameras();
    if (_cameras.isNotEmpty) {
      // Default to the first camera (usually integrated)
      _selectedCameraIndex = 0;
      await _startLocalCamera();
    }
  }

  Future<void> _loadCameras() async {
    try {
      _cameras = await availableCameras();
    } catch (e) {
      print('Error loading cameras: $e');
    }
  }

  Future<void> _switchCamera() async {
    if (Platform.isWindows) return; // TODO: Implement camera switching for Windows native

    if (_cameras.isEmpty) return;
    
    setState(() {
      _selectedCameraIndex = (_selectedCameraIndex + 1) % _cameras.length;
    });
    await _startLocalCamera();
  }

  Future<void> _toggleQuality() async {
    if (Platform.isWindows) return; // TODO: Implement quality toggle for Windows native

    setState(() {
      _isHighQuality = !_isHighQuality;
    });
    await _startLocalCamera();
  }

  void _processCameraImage(CameraImage image) async {
    // Always process if mode is not none, regardless of previous frame state
    // This ensures we don't get stuck if a frame drops
    if (_processingMode == ProcessingMode.none) {
      if (_processedImageNotifier.value != null) {
        _processedImageNotifier.value = null;
      }
      return;
    }

    if (_isProcessingFrame) return;
    _isProcessingFrame = true;

    try {
      // Note: processImageAndEncode now returns Future<Uint8List?>
      final bytes = await ImageProcessingService.instance.processImageAndEncode(image);
      if (mounted) {
         _processedImageNotifier.value = bytes;
      }
    } catch (e) {
      print('Error processing frame: $e');
    } finally {
      _isProcessingFrame = false;
    }
  }

  void _cycleProcessingMode() {
    setState(() {
      final modes = ProcessingMode.values;
      final nextIndex = (_processingMode.index + 1) % modes.length;
      _processingMode = modes[nextIndex];
      
      if (Platform.isWindows) {
        NativeCameraService().setFilter(_processingMode.index);
      } else {
        ImageProcessingService.instance.setProcessingMode(_processingMode);
      }
      
      // Show a snackbar to indicate current mode
      ScaffoldMessenger.of(context).clearSnackBars();
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text('Mode: ${_processingMode.name.toUpperCase()}'),
          duration: const Duration(milliseconds: 500),
        ),
      );
    });
  }

  @override
  void dispose() {
    _controller?.dispose();
    _ipController.dispose();
    _transformationController.dispose();
    super.dispose();
  }

  Future<void> _startLocalCamera() async {
    if (_controller != null) {
      await _controller!.dispose();
    }

    if (_selectedCameraIndex < 0 || _selectedCameraIndex >= _cameras.length) return;

    final camera = _cameras[_selectedCameraIndex];
    final resolution = _isHighQuality ? ResolutionPreset.max : ResolutionPreset.medium;

    _controller = CameraController(
      camera,
      resolution,
      enableAudio: false,
      imageFormatGroup: Platform.isWindows ? ImageFormatGroup.bgra8888 : ImageFormatGroup.yuv420,
    );

    try {
      await _controller!.initialize();
      setState(() {
        _isInitialized = true;
      });
      
      // Start image stream for processing
      if (Platform.isWindows && _controller!.value.isInitialized) {
        // Native Windows implementation handles streaming internally
      }
      
    } catch (e) {
      print('Error starting camera: $e');
      setState(() {
        _isInitialized = false;
      });
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Error starting camera: $e')),
        );
      }
    }
  }

  Future<void> _captureFrame() async {
    if (!Platform.isWindows) return;

    try {
      // 1. Get full frame data
      final service = NativeCameraService();
      final width = service.getFrameWidth();
      final height = service.getFrameHeight();
      final bytes = service.getFrameData();

      if (bytes == null || width == 0 || height == 0) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Failed to capture frame')),
        );
        return;
      }

      // 2. Decode image
      // Note: bytes are RGBA
      final image = img.Image.fromBytes(
        width: width,
        height: height,
        bytes: bytes.buffer,
        numChannels: 4,
        order: img.ChannelOrder.rgba,
      );

      // 3. Calculate crop rect based on transformation
      final matrix = _transformationController.value;
      final scale = matrix.getMaxScaleOnAxis();
      final translation = matrix.getTranslation();
      
      // Viewport size (screen size available for camera)
      final viewportSize = MediaQuery.of(context).size;
      
      // Let's assume "contain" fit.
      double renderWidth = viewportSize.width;
      double renderHeight = viewportSize.width * 9 / 16;
      
      if (renderHeight > viewportSize.height) {
        renderHeight = viewportSize.height;
        renderWidth = renderHeight * 16 / 9;
      }
      
      double visibleX = -translation.x / scale;
      double visibleY = -translation.y / scale;
      double visibleW = viewportSize.width / scale;
      double visibleH = viewportSize.height / scale;
      
      int cropX = (visibleX * (width / renderWidth)).round();
      int cropY = (visibleY * (height / renderHeight)).round();
      int cropW = (visibleW * (width / renderWidth)).round();
      int cropH = (visibleH * (height / renderHeight)).round();
      
      // Clamp
      cropX = cropX.clamp(0, width);
      cropY = cropY.clamp(0, height);
      if (cropX + cropW > width) cropW = width - cropX;
      if (cropY + cropH > height) cropH = height - cropY;
      
      if (cropW <= 0 || cropH <= 0) {
         // Fallback to full image if calculation fails
         cropX = 0; cropY = 0; cropW = width; cropH = height;
      }

      final croppedImage = img.copyCrop(image, x: cropX, y: cropY, width: cropW, height: cropH);
      final pngBytes = img.encodePng(croppedImage);

      setState(() {
        _capturedImages.add((bytes: pngBytes, width: cropW, height: cropH));
      });

      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Captured image ${_capturedImages.length}')),
      );
      
    } catch (e) {
      print('Error capturing frame: $e');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Error capturing frame: $e')),
      );
    }
  }

  Future<void> _exportPdf() async {
    if (_capturedImages.isEmpty) return;

    try {
      final pdf = pw.Document();

      for (final item in _capturedImages) {
        final image = pw.MemoryImage(item.bytes);
        
        // Create a page with the exact dimensions of the image
        pdf.addPage(
          pw.Page(
            pageFormat: PdfPageFormat(item.width.toDouble(), item.height.toDouble()),
            margin: pw.EdgeInsets.zero,
            build: (pw.Context context) {
              return pw.Image(image, fit: pw.BoxFit.cover);
            },
          ),
        );
      }

      String fileName = _pdfNameController.text.trim();
      if (fileName.isEmpty) {
        final now = DateTime.now();
        fileName = "Capture_${now.year}-${now.month.toString().padLeft(2, '0')}-${now.day.toString().padLeft(2, '0')}_${now.hour.toString().padLeft(2, '0')}-${now.minute.toString().padLeft(2, '0')}";
      }
      if (!fileName.toLowerCase().endsWith('.pdf')) {
        fileName += '.pdf';
      }

      String dirPath = _pdfPathController.text.trim();
      if (dirPath.isEmpty) {
        final dir = await getApplicationDocumentsDirectory();
        dirPath = dir.path;
      }

      final file = File("$dirPath/$fileName");
      await file.writeAsBytes(await pdf.save());

      setState(() {
        _capturedImages.clear();
        _pdfNameController.clear();
      });

      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Saved PDF to ${file.path}')),
      );
    } catch (e) {
      print('Error exporting PDF: $e');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Error exporting PDF: $e')),
      );
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Camera Mode'),
        actions: [
          IconButton(
            icon: Icon(_isSidebarOpen ? Icons.chevron_right : Icons.list),
            onPressed: () {
              setState(() {
                _isSidebarOpen = !_isSidebarOpen;
              });
            },
          ),
        ],
      ),
      body: Stack(
        children: [
          // Main Content
          Positioned.fill(
            child: Column(
              children: [
                Expanded(
                  child: Stack(
                    children: [
                      if (Platform.isWindows)
                         InteractiveViewer(
                           transformationController: _transformationController,
                           minScale: 1.0,
                           maxScale: 50.0,
                           child: const NativeCameraView(),
                         )
                      else if (_isInitialized && _controller != null)
                        InteractiveViewer(
                          transformationController: _transformationController,
                          minScale: 1.0,
                          maxScale: 50.0,
                          child: Transform.scale(
                            scaleX: -1,
                            alignment: Alignment.center,
                            child: ValueListenableBuilder<Uint8List?>(
                              valueListenable: _processedImageNotifier,
                              builder: (context, processedImage, child) {
                                if (processedImage != null && _processingMode != ProcessingMode.none) {
                                  return Image.memory(
                                    processedImage,
                                    gaplessPlayback: true,
                                    fit: BoxFit.contain,
                                  );
                                }
                                return CameraPreview(_controller!);
                              },
                            ),
                          ),
                        )
                      else
                        Center(
                          child: Column(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: [
                              if (_cameras.isEmpty && !Platform.isWindows)
                                const Text('No cameras found')
                              else if (_controller == null && !Platform.isWindows)
                                const CircularProgressIndicator()
                              else
                                Column(
                                  children: [
                                    const Icon(Icons.error_outline, size: 48, color: Colors.red),
                                    const SizedBox(height: 16),
                                    const Text('Camera failed to initialize'),
                                    if (!Platform.isWindows)
                                      TextButton(
                                        onPressed: _switchCamera,
                                        child: const Text('Try Next Camera'),
                                      ),
                                  ],
                                ),
                            ],
                          ),
                        ),
                        
                      Positioned(
                        bottom: 20,
                        left: 0,
                        right: 0,
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                          children: [
                            FloatingActionButton(
                              heroTag: 'switch_camera',
                              onPressed: _switchCamera,
                              backgroundColor: Colors.black54,
                              child: const Icon(Icons.switch_camera, color: Colors.white),
                            ),
                            FloatingActionButton(
                              heroTag: 'filters',
                              onPressed: _cycleProcessingMode,
                              backgroundColor: _processingMode != ProcessingMode.none ? Colors.orange : Colors.black54,
                              child: const Icon(Icons.filter_b_and_w, color: Colors.white),
                            ),
                            FloatingActionButton(
                              heroTag: 'max_quality',
                              onPressed: _toggleQuality,
                              backgroundColor: _isHighQuality ? Colors.blue : Colors.black54,
                              child: const Icon(Icons.high_quality, color: Colors.white),
                            ),
                            FloatingActionButton(
                              heroTag: 'capture_frame',
                              onPressed: _captureFrame,
                              backgroundColor: Colors.red,
                              child: const Icon(Icons.camera_alt, color: Colors.white),
                            ),
                            // Export button moved to sidebar, but keeping here as shortcut if sidebar is closed?
                            // User asked for "2 bottoms, one take a picture... combine all the pictures to a one pdf document"
                            // I'll keep it here for now, but maybe hide it if sidebar is open?
                            // Actually, let's keep it.
                            if (_capturedImages.isNotEmpty)
                              FloatingActionButton(
                                heroTag: 'export_pdf_fab',
                                onPressed: _exportPdf,
                                backgroundColor: Colors.green,
                                child: const Icon(Icons.picture_as_pdf, color: Colors.white),
                              ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),

          // Sidebar
          AnimatedPositioned(
            duration: const Duration(milliseconds: 300),
            top: 0,
            bottom: 0,
            right: _isSidebarOpen ? 0 : -350,
            width: 350,
            child: Container(
              decoration: BoxDecoration(
                color: Colors.black,
                boxShadow: [
                  BoxShadow(
                    color: Colors.black.withOpacity(0.5),
                    blurRadius: 5,
                    offset: const Offset(-2, 0),
                  ),
                ],
              ),
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    mainAxisAlignment: MainAxisAlignment.spaceBetween,
                    children: [
                      const Text('Captured Images', style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold, color: Colors.white)),
                      IconButton(
                        icon: const Icon(Icons.close, color: Colors.white),
                        onPressed: () => setState(() => _isSidebarOpen = false),
                      ),
                    ],
                  ),
                  const SizedBox(height: 10),
                  Expanded(
                    child: _capturedImages.isEmpty
                        ? const Center(child: Text('No images captured', style: TextStyle(color: Colors.white70)))
                        : ListView.builder(
                            itemCount: _capturedImages.length,
                            itemBuilder: (context, index) {
                              final item = _capturedImages[index];
                              return Card(
                                color: Colors.blue[900],
                                margin: const EdgeInsets.only(bottom: 8),
                                child: ListTile(
                                  contentPadding: const EdgeInsets.all(8),
                                  leading: Image.memory(
                                    item.bytes,
                                    width: 60,
                                    height: 60,
                                    fit: BoxFit.cover,
                                  ),
                                  title: Text('Image ${index + 1}', style: const TextStyle(color: Colors.white)),
                                  trailing: IconButton(
                                    icon: const Icon(Icons.delete, color: Colors.redAccent),
                                    onPressed: () {
                                      setState(() {
                                        _capturedImages.removeAt(index);
                                      });
                                    },
                                  ),
                                ),
                              );
                            },
                          ),
                  ),
                  const Divider(thickness: 1, color: Colors.white24),
                  const Text('PDF Settings', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold, color: Colors.white)),
                  const SizedBox(height: 8),
                  TextField(
                    controller: _pdfNameController,
                    style: const TextStyle(color: Colors.white),
                    decoration: const InputDecoration(
                      labelText: 'File Name',
                      labelStyle: TextStyle(color: Colors.white70),
                      hintText: 'Default: Capture_YYYY-MM-DD_HH-MM',
                      hintStyle: TextStyle(color: Colors.white30),
                      enabledBorder: OutlineInputBorder(borderSide: BorderSide(color: Colors.white30)),
                      focusedBorder: OutlineInputBorder(borderSide: BorderSide(color: Colors.blue)),
                      isDense: true,
                    ),
                  ),
                  const SizedBox(height: 8),
                  TextField(
                    controller: _pdfPathController,
                    style: const TextStyle(color: Colors.white),
                    decoration: InputDecoration(
                      labelText: 'Save Directory',
                      labelStyle: const TextStyle(color: Colors.white70),
                      enabledBorder: const OutlineInputBorder(borderSide: BorderSide(color: Colors.white30)),
                      focusedBorder: const OutlineInputBorder(borderSide: BorderSide(color: Colors.blue)),
                      isDense: true,
                      suffixIcon: IconButton(
                        icon: const Icon(Icons.more_horiz, color: Colors.white),
                        onPressed: () async {
                          String? selectedDirectory = await FilePicker.platform.getDirectoryPath();
                          if (selectedDirectory != null) {
                            setState(() {
                              _pdfPathController.text = selectedDirectory;
                            });
                          }
                        },
                      ),
                    ),
                  ),
                  const SizedBox(height: 16),
                  SizedBox(
                    width: double.infinity,
                    height: 50,
                    child: ElevatedButton.icon(
                      onPressed: _capturedImages.isNotEmpty ? _exportPdf : null,
                      icon: const Icon(Icons.save_alt),
                      label: const Text('Export PDF'),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.blue,
                        foregroundColor: Colors.white,
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
