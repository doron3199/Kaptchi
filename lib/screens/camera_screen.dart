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

class FilterItem {
  final int id;
  final String name;
  bool isActive;

  FilterItem({required this.id, required this.name, this.isActive = false});
}

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
  bool _isHighQuality = true;
  bool _isInitialized = false;
  
  // Image processing state
  final ValueNotifier<Uint8List?> _processedImageNotifier = ValueNotifier(null);
  bool _isProcessingFrame = false;
  ProcessingMode _processingMode = ProcessingMode.none;
  
  // Captured images for PDF export
  final List<({Uint8List bytes, int width, int height})> _capturedImages = [];

  // Sidebar state
  bool _isSidebarOpen = false;
  bool _isLeftSidebarOpen = false;
  final TextEditingController _pdfNameController = TextEditingController();
  final TextEditingController _pdfPathController = TextEditingController();

  // Filter state
  final List<FilterItem> _filters = [
    FilterItem(id: 8, name: 'Sharpening', isActive: false),
    FilterItem(id: 7, name: 'Contrast Boost (CLAHE)', isActive: false),
    FilterItem(id: 6, name: 'Moving Average', isActive: false),
    FilterItem(id: 5, name: 'Smart Obstacle Removal', isActive: false),
    FilterItem(id: 4, name: 'Smart Whiteboard', isActive: false),
    FilterItem(id: 3, name: 'Blur (Legacy)', isActive: false),
    FilterItem(id: 1, name: 'Invert Colors', isActive: false),
    FilterItem(id: 2, name: 'Whiteboard (Legacy)', isActive: false),
  ];

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
    if (Platform.isWindows) {
      NativeCameraService().switchCamera();
      return;
    }

    if (_cameras.isEmpty) return;
    
    setState(() {
      _selectedCameraIndex = (_selectedCameraIndex + 1) % _cameras.length;
    });
    await _startLocalCamera();
  }

  Future<void> _toggleQuality() async {
    setState(() {
      _isHighQuality = !_isHighQuality;
    });

    if (Platform.isWindows) {
      // Toggle between 1080p and 4K
      if (_isHighQuality) {
        NativeCameraService().setResolution(4096 , 2160);
      } else {
        NativeCameraService().setResolution(1920, 1080);
      }
    } else {
      await _startLocalCamera();
    }
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

  void _updateFilters() {
    if (Platform.isWindows) {
      final activeFilters = _filters
          .where((f) => f.isActive)
          .map((f) => f.id)
          .toList();
      NativeCameraService().setFilterSequence(activeFilters);
    } else {
      // Fallback for non-Windows (not fully implemented for sequence)
      // Just use the first active one or none
      final active = _filters.firstWhere((f) => f.isActive, orElse: () => FilterItem(id: 0, name: 'None'));
      if (active.id == 0) {
        _processingMode = ProcessingMode.none;
      } else {
        _processingMode = ProcessingMode.values[active.id];
      }
      ImageProcessingService.instance.setProcessingMode(_processingMode);
    }
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
        leading: IconButton(
          icon: Icon(_isLeftSidebarOpen ? Icons.chevron_left : Icons.tune),
          onPressed: () {
            setState(() {
              _isLeftSidebarOpen = !_isLeftSidebarOpen;
            });
          },
        ),
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
                            // Removed filter cycle button
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

          // Left Sidebar (Filters)
          AnimatedPositioned(
            duration: const Duration(milliseconds: 300),
            top: 0,
            bottom: 0,
            left: _isLeftSidebarOpen ? 0 : -300,
            width: 300,
            child: Container(
              decoration: BoxDecoration(
                color: Colors.black,
                boxShadow: [
                  BoxShadow(
                    color: Colors.black.withOpacity(0.5),
                    blurRadius: 5,
                    offset: const Offset(2, 0),
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
                      const Text('Filters', style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold, color: Colors.white)),
                      IconButton(
                        icon: const Icon(Icons.close, color: Colors.white),
                        onPressed: () => setState(() => _isLeftSidebarOpen = false),
                      ),
                    ],
                  ),
                  const SizedBox(height: 10),
                  const Text(
                    'Drag to reorder. Top filters apply first.',
                    style: TextStyle(color: Colors.white54, fontSize: 12),
                  ),
                  const SizedBox(height: 10),
                  Expanded(
                    child: ReorderableListView(
                      onReorder: (oldIndex, newIndex) {
                        setState(() {
                          if (oldIndex < newIndex) {
                            newIndex -= 1;
                          }
                          final item = _filters.removeAt(oldIndex);
                          _filters.insert(newIndex, item);
                          _updateFilters();
                        });
                      },
                      children: [
                        for (int index = 0; index < _filters.length; index++)
                          Card(
                            key: ValueKey(_filters[index].id),
                            color: Colors.grey[900],
                            child: SwitchListTile(
                              title: Text(_filters[index].name, style: const TextStyle(color: Colors.white)),
                              value: _filters[index].isActive,
                              onChanged: (bool value) {
                                setState(() {
                                  _filters[index].isActive = value;
                                  _updateFilters();
                                });
                              },
                              secondary: const Icon(Icons.drag_handle, color: Colors.white54),
                            ),
                          ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          ),

          // Right Sidebar (Captured Images)
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
