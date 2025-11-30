import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';
import 'package:image/image.dart' as img;
import 'package:file_picker/file_picker.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:qr_flutter/qr_flutter.dart';
import 'package:wakelock_plus/wakelock_plus.dart';
import 'image_editor_screen.dart';
import '../services/image_processing_service.dart';
import '../widgets/native_camera_view.dart';
import '../services/native_camera_service.dart';
import '../services/webrtc_service.dart';
import '../services/signaling_server.dart';

class FilterItem {
  final int id;
  final String name;
  bool isActive;

  FilterItem({required this.id, required this.name, this.isActive = false});
}

class CameraScreen extends StatefulWidget {
  final String? connectionUrl;
  final int? initialCameraIndex;
  final WebRTCService? preConnectedWebRTCService;
  final SignalingServer? preStartedSignalingServer;

  const CameraScreen({
    super.key, 
    this.connectionUrl,
    this.initialCameraIndex,
    this.preConnectedWebRTCService,
    this.preStartedSignalingServer,
  });

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> {
  final _ipController = TextEditingController();
  final _transformationController = TransformationController();
  late final WebRTCService _webrtcService;
  final _localRenderer = RTCVideoRenderer();
  late final SignalingServer _signalingServer;
  
  CameraController? _controller;
  List<CameraDescription> _cameras = [];
  int _selectedCameraIndex = -1;
  bool _isHighQuality = true;
  bool _isInitialized = false;
  
  // WebRTC / Monitor Mode State
  bool _isWebRTCMode = false;
  String _serverIp = '';
  int _serverPort = 5000;
  List<({String name, String ip})> _availableInterfaces = [];
  Timer? _frameTimer;
  final GlobalKey _videoViewKey = GlobalKey();
  
  // Image processing state
  final ValueNotifier<Uint8List?> _processedImageNotifier = ValueNotifier(null);
  bool _isProcessingFrame = false;
  
  // Zoom state
  double _currentZoom = 1.0;
  final double _phoneMaxZoom = 10.0;
  bool _isDigitalZoomOverride = false;
  double _lockedPhoneZoom = 1.0;
  Offset _viewOffset = Offset.zero;
  double _lastScale = 1.0;
  Offset? _lastFocalPoint;
  
  // Captured images for PDF export
  final List<({Uint8List bytes, int width, int height})> _capturedImages = [];
  int _currentGalleryIndex = 0;

  // Sidebar state
  bool _isSidebarOpen = false;
  bool _isLeftSidebarOpen = false;
  bool _isGalleryFullScreen = false;
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
    
    _webrtcService = widget.preConnectedWebRTCService ?? WebRTCService();
    _signalingServer = widget.preStartedSignalingServer ?? SignalingServer.instance;

    // Enable digital zoom override by default for native cameras (Windows)
    if (Platform.isWindows && widget.preConnectedWebRTCService == null) {
      _isDigitalZoomOverride = true;
    }

    // Handle disconnection on Windows
    if (Platform.isWindows && widget.preConnectedWebRTCService != null) {
      _webrtcService.onIceConnectionStateChange = (state) {
        if (state == RTCIceConnectionState.RTCIceConnectionStateDisconnected ||
            state == RTCIceConnectionState.RTCIceConnectionStateFailed ||
            state == RTCIceConnectionState.RTCIceConnectionStateClosed) {
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(content: Text('Connection lost')),
            );
            Navigator.pop(context);
          }
        }
      };
    }

    _init();
    _setDefaultPath();
    _localRenderer.initialize().then((_) {
      if (mounted && widget.preConnectedWebRTCService != null) {
         setState(() {
             _isWebRTCMode = true;
             if (_webrtcService.remoteStream != null) {
                _localRenderer.srcObject = _webrtcService.remoteStream;
                _startFrameExtraction(_webrtcService.remoteStream!);
             }
         });
         
         _webrtcService.onRemoteStream = (stream) {
            setState(() {
              _localRenderer.srcObject = stream;
            });
            _startFrameExtraction(stream);
         };
      }
    });

    if (widget.connectionUrl != null) {
      WakelockPlus.enable();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _connectToUrl(widget.connectionUrl!);
      });
    }
  }

  Future<void> _startWebRTCServer() async {
    try {
      await _signalingServer.start();
      final interfaces = await _signalingServer.getNetworkInterfaces();
      
      setState(() {
        _serverPort = _signalingServer.port;
        _availableInterfaces = interfaces;
        if (interfaces.isNotEmpty) {
          _serverIp = interfaces[0].ip;
        }
      });

      // Connect to our own local server to listen for the camera
      await _webrtcService.connect('localhost:$_serverPort', false);
      
      _webrtcService.onIceConnectionStateChange = (state) {
        if (state == RTCIceConnectionState.RTCIceConnectionStateDisconnected ||
            state == RTCIceConnectionState.RTCIceConnectionStateFailed ||
            state == RTCIceConnectionState.RTCIceConnectionStateClosed) {
          if (mounted) {
             Navigator.pop(context);
          }
        }
      };
      
      _webrtcService.onRemoteStream = (stream) {
        // Close the QR code dialog if it's open
        if (Navigator.canPop(context)) {
          Navigator.pop(context);
        }
        
        setState(() {
          _localRenderer.srcObject = stream;
        });
        
        // Start pulling frames from the stream for processing
        _startFrameExtraction(stream);
      };

      _showQrCodeDialog();

    } catch (e) {
      print('Error starting WebRTC server: $e');
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Error starting server: $e')),
        );
      }
    }
  }

  void _startFrameExtraction(MediaStream stream) {
    _frameTimer?.cancel();
    // Extract frames at ~15fps (sufficient for processing without killing CPU)
    _frameTimer = Timer.periodic(const Duration(milliseconds: 66), (timer) async {
      // Check if any filter is active
      bool hasActiveFilters = _filters.any((f) => f.isActive);
      
      if (!hasActiveFilters) {
        if (_processedImageNotifier.value != null) {
          _processedImageNotifier.value = null;
        }
        return;
      }

      try {
        // Capture frame from the RepaintBoundary wrapping the RTCVideoView
        final boundary = _videoViewKey.currentContext?.findRenderObject() as RenderRepaintBoundary?;
        if (boundary == null) return;

        // Capture as image
        final image = await boundary.toImage();
        
        // Get byte data (RGBA)
        final byteData = await image.toByteData(format: ui.ImageByteFormat.rawRgba);
        if (byteData == null) return;

        // Convert to CameraImage-like structure or pass directly to service
        // Our service expects CameraImage, but we can overload or create a helper
        // Actually, ImageProcessingService.processImageAndEncode expects CameraImage.
        // We should add a method to process raw bytes.
        
        // Let's modify ImageProcessingService to accept raw bytes too, or just use the internal logic here.
        // Since we can't easily modify the service interface in this step without reading it again,
        // let's assume we can add a helper or just call the C++ function directly if we exposed it.
        // Wait, ImageProcessingService exposes processImageAndEncode which takes CameraImage.
        // We need to extend ImageProcessingService to handle raw bytes.
        
        // For now, let's use a workaround: Create a fake CameraImage? No, that's hard.
        // Let's add a new method to ImageProcessingService in the next step.
        // Assuming we will add `processRawRgba` to ImageProcessingService.
        
        final processedBytes = await ImageProcessingService.instance.processRawRgba(
          byteData.buffer.asUint8List(), 
          image.width, 
          image.height
        );
        
        if (mounted) {
          _processedImageNotifier.value = processedBytes;
        }
      } catch (e) {
        print('Error extracting frame: $e');
      }
    });
  }

  void _showQrCodeDialog() {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Connect Mobile Camera'),
        content: SizedBox(
          width: 300,
          height: 400, // Increased height to prevent overflow
          child: SingleChildScrollView( // Added scroll view to prevent overflow
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                if (_availableInterfaces.isNotEmpty)
                  Container(
                    color: Colors.white,
                    padding: const EdgeInsets.all(16),
                    child: QrImageView(
                      data: 'ws://$_serverIp:$_serverPort',
                      version: QrVersions.auto,
                      size: 200.0,
                    ),
                  ),
                const SizedBox(height: 16),
                Text('Scan with Kaptchi mobile app\nws://$_serverIp:$_serverPort', textAlign: TextAlign.center),
                const SizedBox(height: 16),
                DropdownButton<String>(
                  value: _serverIp,
                  isExpanded: true,
                  items: _availableInterfaces.map((i) {
                    return DropdownMenuItem(
                      value: i.ip,
                      child: Text('${i.name} (${i.ip})'),
                    );
                  }).toList(),
                  onChanged: (val) {
                    if (val != null) {
                      setState(() {
                        _serverIp = val;
                      });
                      Navigator.pop(context);
                      _showQrCodeDialog();
                    }
                  },
                ),
              ],
            ),
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Close'),
          ),
        ],
      ),
    );
  }

  Future<void> _connectToUrl(String url) async {
    WakelockPlus.enable();
    _webrtcService.onError = (error) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Connection Error: $error'),
            backgroundColor: Colors.red,
            duration: const Duration(seconds: 5),
          ),
        );
      }
    };

    _webrtcService.onIceConnectionStateChange = (state) {
      if (mounted) {
        if (state == RTCIceConnectionState.RTCIceConnectionStateConnected) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Connected!'),
              backgroundColor: Colors.green,
            ),
          );
        } else if (state == RTCIceConnectionState.RTCIceConnectionStateFailed) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Connection Failed'),
              backgroundColor: Colors.red,
            ),
          );
        }
      }
    };

    await _webrtcService.connect(url, true);
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
      // Initialize image processing service
      await ImageProcessingService.instance.initialize();

      if (widget.initialCameraIndex != null) {
        _selectedCameraIndex = widget.initialCameraIndex!;
        NativeCameraService().selectCamera(_selectedCameraIndex);
      }
      return;
    }

    if (widget.connectionUrl != null) return;
    
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
      // Fetch available cameras for the dropdown
      List<CameraDescription> cameras = [];
      try {
        cameras = await availableCameras();
      } catch (e) {
        print('Error getting cameras: $e');
      }

      showModalBottomSheet(
        context: context,
        builder: (context) => Container(
          padding: const EdgeInsets.all(16),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Text('Select Video Source', style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold)),
              const SizedBox(height: 16),
              
              // Local Cameras
              if (cameras.isNotEmpty) ...[
                const Align(
                  alignment: Alignment.centerLeft,
                  child: Text('Local Cameras', style: TextStyle(fontWeight: FontWeight.bold, color: Colors.grey)),
                ),
                ...cameras.asMap().entries.map((entry) {
                  final index = entry.key;
                  final camera = entry.value;
                  // Clean up camera name by removing ID part in angle brackets <...>
                  final cleanName = camera.name.replaceAll(RegExp(r'<[^>]*>'), '').trim();
                  
                  return ListTile(
                    leading: const Icon(Icons.camera),
                    title: Text(cleanName),
                    onTap: () async {
                      Navigator.pop(context);
                      setState(() {
                        _isWebRTCMode = false;
                        _isDigitalZoomOverride = true; // Enable by default for local cameras
                        _lockedPhoneZoom = 1.0;
                        _currentZoom = 1.0;
                        _viewOffset = Offset.zero;
                      });
                      _webrtcService.disconnect();
                      await _signalingServer.stop();
                      NativeCameraService().selectCamera(index);
                    },
                  );
                }),
                const Divider(),
              ],

              ListTile(
                leading: const Icon(Icons.wifi_tethering),
                title: const Text('WebRTC Stream (Mobile Camera)'),
                onTap: () {
                  Navigator.pop(context);
                  setState(() {
                    _isWebRTCMode = true;
                    _isDigitalZoomOverride = false; // Disable by default for mobile
                    _currentZoom = 1.0;
                    _viewOffset = Offset.zero;
                  });
                  NativeCameraService().stop();
                  _startWebRTCServer();
                },
              ),
            ],
          ),
        ),
      );
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
    if (!_filters.any((f) => f.isActive)) {
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

  void _sendZoomCommand(double zoom) {
    // We can send this via the signaling server as a custom message type
    // The mobile side needs to listen for this.
    // Since we don't have a direct data channel set up in this simple example,
    // we can piggyback on the signaling server's broadcast mechanism.
    // However, the signaling server broadcasts to *other* clients.
    // If we are the receiver (Windows), we are a client. The sender (Android) is also a client.
    // So sending to the signaling server should reach the Android device.
    
    // We need to access the socket in WebRTCService or SignalingServer to send.
    // WebRTCService has _send but it wraps in a specific format.
    // Let's add a generic send method to WebRTCService or expose the socket.
    
    // Better approach: Add a method to WebRTCService to send a control message.
    _webrtcService.sendControlMessage('zoom', {'level': zoom});
  }

  void _updateFilters() {
    // 1. Get active filters
    final activeFilters = _filters
        .where((f) => f.isActive)
        .map((f) => f.id)
        .toList();

    // 2. Update Native Camera Service (Windows Local Camera)
    if (Platform.isWindows) {
      NativeCameraService().setFilterSequence(activeFilters);
    }

    // 3. Update Image Processing Service (WebRTC / Non-Windows)
    List<ProcessingMode> modes = [];
    for (var id in activeFilters) {
      if (id >= 0 && id < ProcessingMode.values.length) {
        modes.add(ProcessingMode.values[id]);
      }
    }
    
    ImageProcessingService.instance.setProcessingModes(modes);
  }

  @override
  void dispose() {
    WakelockPlus.disable();
    _controller?.dispose();
    _ipController.dispose();
    _transformationController.dispose();
    _webrtcService.disconnect();
    // _signalingServer.stop(); // NEVER STOP THE SERVER
    _frameTimer?.cancel();
    _localRenderer.dispose();
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
      Uint8List? finalBytes;
      int finalWidth = 0;
      int finalHeight = 0;

      if (_isWebRTCMode) {
        // If we have a processed (filtered) image, use it
        bool hasActiveFilters = _filters.any((f) => f.isActive);
        if (hasActiveFilters && _processedImageNotifier.value != null) {
          finalBytes = _processedImageNotifier.value;
          // Decode to get dimensions
          final decoded = img.decodeImage(finalBytes!);
          if (decoded != null) {
            finalWidth = decoded.width;
            finalHeight = decoded.height;
          }
        } else {
          // Otherwise capture the raw stream from the view
          final boundary = _videoViewKey.currentContext?.findRenderObject() as RenderRepaintBoundary?;
          if (boundary == null) {
             ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(content: Text('Failed to capture WebRTC frame')),
            );
            return;
          }
          
          final image = await boundary.toImage();
          final byteData = await image.toByteData(format: ui.ImageByteFormat.png);
          
          if (byteData != null) {
            finalBytes = byteData.buffer.asUint8List();
            finalWidth = image.width;
            finalHeight = image.height;
          }
        }
      } else {
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
        finalBytes = img.encodePng(croppedImage);
        finalWidth = cropW;
        finalHeight = cropH;
      }

      if (finalBytes != null) {
        setState(() {
          _capturedImages.add((bytes: finalBytes!, width: finalWidth, height: finalHeight));
        });

        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Captured image ${_capturedImages.length}')),
        );
      }
      
    } catch (e) {
      print('Error capturing frame: $e');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Error capturing frame: $e')),
      );
    }
  }

  Future<void> _openEditor() async {
    if (_capturedImages.isEmpty) return;
    if (_currentGalleryIndex >= _capturedImages.length) {
        _currentGalleryIndex = _capturedImages.length - 1;
    }
    
    final item = _capturedImages[_currentGalleryIndex];
    final result = await Navigator.push(
      context,
      MaterialPageRoute(
        builder: (context) => ImageEditorScreen(imageBytes: item.bytes),
      ),
    );

    if (result != null && result is Uint8List) {
      final decoded = img.decodeImage(result);
      if (decoded != null) {
        setState(() {
          _capturedImages[_currentGalleryIndex] = (
            bytes: result,
            width: decoded.width,
            height: decoded.height,
          );
        });
      }
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

  Future<void> _scanQrCode() async {
    final result = await Navigator.push(
      context,
      MaterialPageRoute(
        builder: (context) => Scaffold(
          appBar: AppBar(title: const Text('Scan QR Code')),
          body: MobileScanner(
            onDetect: (capture) {
              final List<Barcode> barcodes = capture.barcodes;
              for (final barcode in barcodes) {
                if (barcode.rawValue != null) {
                  Navigator.pop(context, barcode.rawValue);
                  return;
                }
              }
            },
          ),
        ),
      ),
    );

    if (result != null && result is String) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Connecting to $result...')),
        );
      }
      
      // Stop the local camera preview to free up the resource for WebRTC
      if (_controller != null) {
        await _controller!.dispose();
        setState(() {
          _controller = null;
        });
      }

      // Add a small delay to ensure camera is released by MobileScanner and Controller
      await Future.delayed(const Duration(milliseconds: 500));

      _webrtcService.onError = (error) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text('Connection Error: $error'),
              backgroundColor: Colors.red,
              duration: const Duration(seconds: 5),
            ),
          );
        }
      };

      _webrtcService.onIceConnectionStateChange = (state) {
        if (mounted) {
          if (state == RTCIceConnectionState.RTCIceConnectionStateConnected) {
             ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(
                content: Text('Connected!'),
                backgroundColor: Colors.green,
              ),
            );
          } else if (state == RTCIceConnectionState.RTCIceConnectionStateFailed) {
             ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(
                content: Text('Connection Failed'),
                backgroundColor: Colors.red,
              ),
            );
          }
        }
      };

      await _webrtcService.connect(result, true);
      
      if (_webrtcService.localStream != null) {
        setState(() {
          _isWebRTCMode = true;
          _localRenderer.srcObject = _webrtcService.localStream;
        });
        _startFrameExtraction(_webrtcService.localStream!);
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    if (widget.connectionUrl != null) {
      return Scaffold(
        appBar: AppBar(title: const Text('Transmitting')),
        body: Center(
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              const Text('Transmitting Video...', style: TextStyle(fontSize: 20)),
              const SizedBox(height: 40),
              
              ElevatedButton.icon(
                onPressed: () {
                  WakelockPlus.disable();
                  _webrtcService.disconnect();
                  Navigator.pop(context);
                },
                icon: const Icon(Icons.stop),
                label: const Text('Stop Transmission'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.red,
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 16),
                ),
              ),
            ],
          ),
        ),
      );
    }

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
          if (!Platform.isWindows)
            IconButton(
              icon: const Icon(Icons.qr_code_scanner),
              onPressed: _scanQrCode,
            ),
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
                        LayoutBuilder(
                          builder: (context, constraints) {
                            return Listener(
                              onPointerSignal: (event) {
                                if (event is PointerScrollEvent) {
                                  // Calculate new zoom based on scroll direction
                                  // Scroll down (positive) -> Zoom out
                                  // Scroll up (negative) -> Zoom in
                                  double scaleFactor = event.scrollDelta.dy > 0 ? 0.9 : 1.1;
                                  
                                  // Calculate min zoom: 1.0 normally, or locked zoom if override is on
                                  double minZoom = _isDigitalZoomOverride ? _lockedPhoneZoom : 1.0;
                                  double newTotalZoom = (_currentZoom * scaleFactor).clamp(minZoom, 100.0);
                                  
                                  // Calculate visual scales for offset adjustment
                                  double oldVisualZoom;
                                  double newVisualZoom;
                                  
                                  if (_isDigitalZoomOverride) {
                                    oldVisualZoom = _currentZoom / _lockedPhoneZoom;
                                    newVisualZoom = newTotalZoom / _lockedPhoneZoom;
                                  } else {
                                    oldVisualZoom = _currentZoom > _phoneMaxZoom ? _currentZoom / _phoneMaxZoom : 1.0;
                                    newVisualZoom = newTotalZoom > _phoneMaxZoom ? newTotalZoom / _phoneMaxZoom : 1.0;
                                  }
                                  
                                  double visualScaleDelta = newVisualZoom / oldVisualZoom;

                                  setState(() {
                                    _currentZoom = newTotalZoom;
                                    
                                    if (newVisualZoom <= 1.0) {
                                      _viewOffset = Offset.zero;
                                    } else {
                                      // Apply Zoom around cursor position
                                      // Convert cursor position to be relative to center
                                      Offset center = Offset(constraints.maxWidth / 2, constraints.maxHeight / 2);
                                      Offset focalPointFromCenter = event.localPosition - center;
                                      
                                      // Adjust offset to keep focal point stable during zoom
                                      _viewOffset -= (focalPointFromCenter - _viewOffset) * (visualScaleDelta - 1);
                                      
                                      // Clamp Offset
                                      double maxDx = (constraints.maxWidth * newVisualZoom - constraints.maxWidth) / 2;
                                      double maxDy = (constraints.maxHeight * newVisualZoom - constraints.maxHeight) / 2;
                                      _viewOffset = Offset(
                                        _viewOffset.dx.clamp(-maxDx, maxDx),
                                        _viewOffset.dy.clamp(-maxDy, maxDy),
                                      );
                                    }
                                    
                                    // Send command to phone (clamped)
                                    if (!_isDigitalZoomOverride && _isWebRTCMode) {
                                      _sendZoomCommand(_currentZoom.clamp(1.0, _phoneMaxZoom));
                                    } else if (_isDigitalZoomOverride && _isWebRTCMode) {
                                      // Keep phone locked at the override level
                                      _sendZoomCommand(_lockedPhoneZoom);
                                    }
                                  });
                                }
                              },
                              child: GestureDetector(
                                onScaleStart: (details) {
                                  _lastScale = 1.0;
                                  _lastFocalPoint = details.localFocalPoint;
                                },
                                onScaleUpdate: (details) {
                                  if (_lastFocalPoint == null) return;

                                  // 1. Calculate Zoom
                                  double scaleDelta = details.scale / _lastScale;
                                  _lastScale = details.scale;
                                  
                                  // Calculate min zoom: 1.0 normally, or locked zoom if override is on
                                  double minZoom = _isDigitalZoomOverride ? _lockedPhoneZoom : 1.0;
                                  double newTotalZoom = (_currentZoom * scaleDelta).clamp(minZoom, 100.0);
                                  
                                  // Calculate visual scales for offset adjustment
                                  double oldVisualZoom;
                                  double newVisualZoom;
                                  
                                  if (_isDigitalZoomOverride) {
                                    oldVisualZoom = _currentZoom / _lockedPhoneZoom;
                                    newVisualZoom = newTotalZoom / _lockedPhoneZoom;
                                  } else {
                                    oldVisualZoom = _currentZoom > _phoneMaxZoom ? _currentZoom / _phoneMaxZoom : 1.0;
                                    newVisualZoom = newTotalZoom > _phoneMaxZoom ? newTotalZoom / _phoneMaxZoom : 1.0;
                                  }
                                  
                                  double visualScaleDelta = newVisualZoom / oldVisualZoom;

                                  // 2. Calculate Pan
                                  Offset focalPointDelta = details.localFocalPoint - _lastFocalPoint!;
                                  _lastFocalPoint = details.localFocalPoint;

                                  setState(() {
                                    _currentZoom = newTotalZoom;
                                    
                                    if (newVisualZoom <= 1.0) {
                                      _viewOffset = Offset.zero;
                                    } else {
                                      // Apply Pan
                                      _viewOffset += focalPointDelta;
                                      
                                      // Apply Zoom around focal point
                                      // Convert focal point to be relative to center
                                      Offset center = Offset(constraints.maxWidth / 2, constraints.maxHeight / 2);
                                      Offset focalPointFromCenter = details.localFocalPoint - center;
                                      
                                      // Adjust offset to keep focal point stable during zoom
                                      _viewOffset -= (focalPointFromCenter - _viewOffset) * (visualScaleDelta - 1);
                                      
                                      // Clamp Offset
                                      double maxDx = (constraints.maxWidth * newVisualZoom - constraints.maxWidth) / 2;
                                      double maxDy = (constraints.maxHeight * newVisualZoom - constraints.maxHeight) / 2;
                                      _viewOffset = Offset(
                                        _viewOffset.dx.clamp(-maxDx, maxDx),
                                        _viewOffset.dy.clamp(-maxDy, maxDy),
                                      );
                                    }
                                    
                                    // Send command to phone (clamped)
                                    if (!_isDigitalZoomOverride && _isWebRTCMode) {
                                      _sendZoomCommand(_currentZoom.clamp(1.0, _phoneMaxZoom));
                                    } else if (_isDigitalZoomOverride && _isWebRTCMode) {
                                      // Keep phone locked at the override level
                                      _sendZoomCommand(_lockedPhoneZoom);
                                    }
                                  });
                                },
                                child: ClipRect(
                                  child: LayoutBuilder(
                                    builder: (context, _) {
                                      // Calculate effective zooms based on override mode
                                      double effectivePhoneZoom = _isDigitalZoomOverride ? _lockedPhoneZoom : _currentZoom.clamp(1.0, _phoneMaxZoom);
                                      double effectiveVisualZoom = _currentZoom / effectivePhoneZoom;

                                      return Transform(
                                        transform: Matrix4.identity()
                                          ..translate(_viewOffset.dx, _viewOffset.dy)
                                          ..scale(effectiveVisualZoom),
                                        alignment: Alignment.center,
                                        child: _isWebRTCMode 
                                           ? Stack(
                                               children: [
                                                 // Hidden RepaintBoundary for capture
                                                 Opacity(
                                                   opacity: 1.0, 
                                                   child: RepaintBoundary(
                                                     key: _videoViewKey,
                                                     child: RTCVideoView(_localRenderer, objectFit: RTCVideoViewObjectFit.RTCVideoViewObjectFitContain),
                                                   ),
                                                 ),
                                                 // Processed overlay
                                                 ValueListenableBuilder<Uint8List?>(
                                                   valueListenable: _processedImageNotifier,
                                                   builder: (context, processedImage, child) {
                                                     bool hasActiveFilters = _filters.any((f) => f.isActive);
                                                     if (processedImage != null && hasActiveFilters) {
                                                       return Opacity(
                                                         opacity: 0.995,
                                                         child: Image.memory(
                                                           processedImage,
                                                           gaplessPlayback: true,
                                                           fit: BoxFit.contain,
                                                           width: double.infinity,
                                                           height: double.infinity,
                                                         ),
                                                       );
                                                     }
                                                     return const SizedBox.shrink();
                                                   },
                                                 ),
                                               ],
                                             )
                                           : const NativeCameraView(),
                                      );
                                    }
                                  ),
                                ),
                              ),
                            );
                          }
                        )
                      else if (_isInitialized && _controller != null)
                        InteractiveViewer(
                          transformationController: _transformationController,
                          minScale: 1.0,
                          maxScale: 50.0,
                          child: ValueListenableBuilder<Uint8List?>(
                            valueListenable: _processedImageNotifier,
                            builder: (context, processedImage, child) {
                              bool hasActiveFilters = _filters.any((f) => f.isActive);
                              if (processedImage != null && hasActiveFilters) {
                                return Image.memory(
                                  processedImage,
                                  gaplessPlayback: true,
                                  fit: BoxFit.contain,
                                );
                              }
                              return CameraPreview(_controller!);
                            },
                          ),
                        )
                      else if (_localRenderer.srcObject != null)
                         InteractiveViewer(
                           transformationController: _transformationController,
                           minScale: 1.0,
                           maxScale: 50.0,
                           child: RTCVideoView(_localRenderer, objectFit: RTCVideoViewObjectFit.RTCVideoViewObjectFitContain),
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
                            // Add the Pan/Digital Zoom Override Button
                            if (_isWebRTCMode)
                              FloatingActionButton(
                                heroTag: 'pan_mode',
                                onPressed: () {
                                  setState(() {
                                    _isDigitalZoomOverride = !_isDigitalZoomOverride;
                                    if (_isDigitalZoomOverride) {
                                      // Lock the phone zoom at the current level
                                      _lockedPhoneZoom = _currentZoom.clamp(1.0, _phoneMaxZoom);
                                      // We don't reset _currentZoom, we just continue from here
                                      // Ensure we send the lock command once
                                      _sendZoomCommand(_lockedPhoneZoom);
                                    } else {
                                      // Restore phone zoom control
                                      _sendZoomCommand(_currentZoom.clamp(1.0, _phoneMaxZoom));
                                    }
                                  });
                                },
                                backgroundColor: _isDigitalZoomOverride ? Colors.blue : Colors.black54,
                                child: Icon(_isDigitalZoomOverride ? Icons.lock : Icons.lock_open, color: Colors.white),
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
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),

          // Edge Gesture Detectors
          Positioned(
            left: 0,
            top: 0,
            bottom: 0,
            width: 120,
            child: GestureDetector(
              behavior: HitTestBehavior.translucent,
              onHorizontalDragEnd: (details) {
                if (details.primaryVelocity! > 300) {
                  setState(() => _isLeftSidebarOpen = true);
                }
              },
              child: Container(color: Colors.transparent),
            ),
          ),
          Positioned(
            right: 0,
            top: 0,
            bottom: 0,
            width: 120,
            child: GestureDetector(
              behavior: HitTestBehavior.translucent,
              onHorizontalDragEnd: (details) {
                if (details.primaryVelocity! < -300) {
                  setState(() => _isSidebarOpen = true);
                }
              },
              child: Container(color: Colors.transparent),
            ),
          ),

          // Left Sidebar (Filters)
          AnimatedPositioned(
            duration: const Duration(milliseconds: 300),
            top: 0,
            bottom: 0,
            left: _isLeftSidebarOpen ? 0 : -300,
            width: 300,
            child: GestureDetector(
              onHorizontalDragEnd: (details) {
                if (details.primaryVelocity! < -300) {
                  setState(() => _isLeftSidebarOpen = false);
                }
              },
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
          ),

          // Right Sidebar (Captured Images)
          AnimatedPositioned(
            duration: const Duration(milliseconds: 300),
            top: 0,
            bottom: 0,
            right: _isSidebarOpen ? 0 : (_isGalleryFullScreen ? -MediaQuery.of(context).size.width : -350),
            width: _isGalleryFullScreen ? MediaQuery.of(context).size.width : 350,
            child: GestureDetector(
              onHorizontalDragEnd: (details) {
                if (details.primaryVelocity! < -300) { // Swipe Left
                  if (!_isGalleryFullScreen) {
                    setState(() {
                      _isGalleryFullScreen = true;
                    });
                  }
                } else if (details.primaryVelocity! > 300) { // Swipe Right
                  if (_isGalleryFullScreen) {
                    setState(() {
                      _isGalleryFullScreen = false;
                    });
                  } else {
                    setState(() {
                      _isSidebarOpen = false;
                    });
                  }
                }
              },
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
                        Row(
                          children: [
                            if (_isGalleryFullScreen && _capturedImages.isNotEmpty)
                              IconButton(
                                icon: const Icon(Icons.edit, color: Colors.white),
                                onPressed: _openEditor,
                              ),
                            IconButton(
                              icon: Icon(_isGalleryFullScreen ? Icons.fullscreen_exit : Icons.close, color: Colors.white),
                              onPressed: () {
                                if (_isGalleryFullScreen) {
                                  setState(() => _isGalleryFullScreen = false);
                                } else {
                                  setState(() => _isSidebarOpen = false);
                                }
                              },
                            ),
                          ],
                        ),
                      ],
                    ),
                  const SizedBox(height: 10),
                  Expanded(
                    child: _capturedImages.isEmpty
                        ? const Center(child: Text('No images captured', style: TextStyle(color: Colors.white70)))
                        : _isGalleryFullScreen
                            ? PageView.builder(
                                scrollDirection: Axis.vertical,
                                itemCount: _capturedImages.length,
                                onPageChanged: (index) {
                                  setState(() {
                                    _currentGalleryIndex = index;
                                  });
                                },
                                itemBuilder: (context, index) {
                                  final item = _capturedImages[index];
                                  return InteractiveViewer(
                                    minScale: 1.0,
                                    maxScale: 5.0,
                                    child: Center(
                                      child: Image.memory(
                                        item.bytes,
                                        fit: BoxFit.contain,
                                      ),
                                    ),
                                  );
                                },
                              )
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
                  if (!_isGalleryFullScreen) ...[
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
                ],
              ),
            ),
          ),
          ),
        ],
      ),
    );
  }
}
