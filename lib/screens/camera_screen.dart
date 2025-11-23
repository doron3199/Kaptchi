import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart';
import '../services/image_processing_service.dart';

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

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    // If we are on Windows, we want to enable local processing capabilities
    if (Platform.isWindows) {
      await ImageProcessingService.instance.initialize();
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
      ImageProcessingService.instance.setProcessingMode(_processingMode);
      
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
        try {
          // Small delay to ensure camera is ready
          await Future.delayed(const Duration(milliseconds: 500));
          await _controller!.startImageStream((CameraImage img) {
            print("Captured frame size: ${img.width}x${img.height}");

            // _processCameraImage);
          });
        } catch (e) {
          print('Warning: Failed to start image stream: $e');
        }
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

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Camera Mode')),
      body: Column(
        children: [
          // Note: WebRTC streaming is temporarily disabled in this view 
          // as we switched to package:camera for better local control.
          // Streaming logic will need to be adapted to read from CameraController.
          
          Expanded(
            child: Stack(
              children: [
                if (_isInitialized && _controller != null)
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
                        if (_cameras.isEmpty)
                          const Text('No cameras found')
                        else if (_controller == null)
                          const CircularProgressIndicator()
                        else
                          Column(
                            children: [
                              const Icon(Icons.error_outline, size: 48, color: Colors.red),
                              const SizedBox(height: 16),
                              const Text('Camera failed to initialize'),
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
                    ],
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
