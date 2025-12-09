import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart' as c;
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';
import 'package:image/image.dart' as img;
import 'package:file_picker/file_picker.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:qr_flutter/qr_flutter.dart';
import 'package:wakelock_plus/wakelock_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import '../services/camera_owner_coordinator.dart';
import 'image_editor_screen.dart';
import '../services/media_server_service.dart';
import '../services/image_processing_service.dart';
import '../widgets/native_camera_view.dart';
import '../services/native_camera_service.dart';
import '../services/rtmp_service.dart';
import '../services/raw_socket_service.dart';
import 'package:vector_math/vector_math_64.dart' as vm;
import '../widgets/camera_sidebars.dart';
import '../widgets/camera_stream_view.dart';

class CameraScreen extends StatefulWidget {
  final String? connectionUrl;
  final int? initialCameraIndex;
  final String? initialStreamUrl;

  const CameraScreen({
    super.key,
    this.connectionUrl,
    this.initialCameraIndex,
    this.initialStreamUrl,
  });

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen>
    with WidgetsBindingObserver {
  final _transformationController = TransformationController();

  c.CameraController? _controller;
  List<c.CameraDescription> _cameras = [];
  int _selectedCameraIndex = -1;
  bool _isHighQuality = true;
  bool _isInitialized = false;

  // Remote / Stream Mode State
  bool _isStreamMode = false;
  String _serverIp = '';
  List<({String name, String ip})> _availableInterfaces = [];
  Timer? _frameTimer;
  final GlobalKey _videoViewKey = GlobalKey();

  // Image processing state
  final ValueNotifier<Uint8List?> _processedImageNotifier = ValueNotifier(null);

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
  String? _activeStreamUrl;
  bool _pendingRtmpResume = false;

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
    FilterItem(id: 11, name: 'Person Removal (AI)', isActive: false),
    FilterItem(id: 3, name: 'Blur (Legacy)', isActive: false),
    FilterItem(id: 1, name: 'Invert Colors', isActive: false),
    FilterItem(id: 2, name: 'Whiteboard (Legacy)', isActive: false),
  ];

  bool _isQrDialogVisible = false;
  StreamSubscription? _mediaServerSubscription;
  StreamSubscription? _streamStoppedSubscription;

  double _minAvailableZoom = 1.0;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // Enable digital zoom override by default for native cameras (Windows)
    if (Platform.isWindows) {
      _isDigitalZoomOverride = true;
    }

    // Setup Raw Socket Listener (Android)
    if (Platform.isAndroid) {
      RawSocketService.instance.onControlMessage = (command, data) {
        if (command == 'zoom') {
          double? level = data['level'];
          if (level != null) {
            debugPrint('Received zoom command: $level');
            if (RtmpService.instance.isStreaming) {
              RtmpService.instance.setZoom(level);
            }
            if (_controller != null) {
              _controller!.setZoomLevel(level);
            }
          }
        }
      };
    }

    // Handle disconnection on Windows
    if (Platform.isWindows) {
      RawSocketService.instance.startServer();



      // Auto-close QR dialog when RTMP stream starts
      _mediaServerSubscription = MediaServerService.instance.onStreamStarted.listen((path) async {
        debugPrint('Stream started: $path');
        if (mounted) {
          if (_isQrDialogVisible) {
            debugPrint('Closing QR Dialog');
            Navigator.of(context, rootNavigator: true).pop();
            // Give the dialog a moment to close before starting the heavy stream initialization
            await Future.delayed(const Duration(milliseconds: 300));
          }
          
          // Automatically connect if we are waiting for a stream
          // This handles the case where we just showed the QR code
          if (Platform.isWindows && !_isStreamMode) {
             final url = 'rtmp://localhost/$path';
             
             setState(() {
               _isStreamMode = true;
             });
             
             // Start Raw Socket server for zoom control
             RawSocketService.instance.startServer().then((_) {
                _connectToStream(url);
             });
          }
        }
      });

      // Listen for stream stop events
      _streamStoppedSubscription = MediaServerService.instance.onStreamStopped.listen((path) {
        if (path == 'live/stream' && mounted) {
           // If we are currently viewing this stream, go back
           if (_isStreamMode && (_activeStreamUrl?.contains(path) ?? false)) {
              ScaffoldMessenger.of(context).showSnackBar(
                 const SnackBar(content: Text('Stream disconnected')),
              );
              // If we are in the camera screen, pop back to home
              // Assuming CameraScreen is pushed on top of HomeScreen
              if (Navigator.canPop(context)) {
                Navigator.pop(context);
              }
           }
        }
      });
    }

    _init();
    _setDefaultPath();

    if (widget.connectionUrl != null) {
      WakelockPlus.enable();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _connectToUrl(widget.connectionUrl!);
      });
    } else if (widget.initialStreamUrl != null) {
        // Ensure we are in "Remote/Stream" mode so the view renders correctly
        // This is critical for Windows to show the player instead of the camera
        _isStreamMode = true;
        WidgetsBinding.instance.addPostFrameCallback((_) {
          _connectToStream(widget.initialStreamUrl!);
        });
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (!Platform.isAndroid && !Platform.isIOS) return;

    if (state == AppLifecycleState.inactive ||
        state == AppLifecycleState.paused) {
      unawaited(_suspendRtmpStreamForLifecycle());
    } else if (state == AppLifecycleState.resumed) {
      unawaited(_resumeRtmpStreamIfNeeded());
    }
  }

  Future<void> _connectToStream(String url) async {
    _activeStreamUrl = url;
    WakelockPlus.enable(); // Keep screen on
    await _releaseLocalCameraResources();

    if (mounted) {
      setState(() {
        _isStreamMode = true;
      });
    }

    if (Platform.isWindows) {
      // Ensure clean state with a small delay
      NativeCameraService().stop();
      await Future.delayed(const Duration(milliseconds: 200));
      
      NativeCameraService().startStream(url);

    }
  }

  Future<void> _releaseLocalCameraResources() async {
    if (_controller != null) {
      final controller = _controller!;
      _controller = null;
      try {
        await controller.dispose();
      } catch (e) {
        debugPrint('Error disposing CameraController: $e');
      }
    }
    if (mounted) {
      setState(() {
        _isInitialized = false;
      });
    } else {
      _isInitialized = false;
    }

    if (Platform.isWindows) {
      NativeCameraService().stop();
    }
  }

  bool get _supportsMobileRtmp => Platform.isAndroid;

  Future<void> _startMobileRtmpStreaming(String url) async {
    if (!_supportsMobileRtmp) return;

    // Request permissions explicitly
    Map<Permission, PermissionStatus> statuses = await [
      Permission.camera,
      Permission.microphone,
    ].request();

    if (statuses[Permission.camera] != PermissionStatus.granted ||
        statuses[Permission.microphone] != PermissionStatus.granted) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(
            content: Text(
              'Camera and Microphone permissions are required for streaming',
            ),
          ),
        );
      }
      return;
    }

    // Force a frame to ensure the preview is built
    if (mounted) {
      setState(() {});
      await Future.delayed(const Duration(milliseconds: 500));
    }

    try {
      debugPrint('Starting RTMP stream to $url');

      // Start the HaishinKit service
      await RtmpService.instance.startStream(url);

      debugPrint('RTMP stream init initiated');

      if (mounted) {
        setState(() {
          _isStreamMode = true;
          // Set max zoom for RTMP since we can't query it easily from HaishinKit yet
        });
      }
    } catch (e) {
      debugPrint('Failed to start RTMP streaming: $e');
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('Failed to start stream: $e')));
      }
    }
  }

  Future<void> _stopMobileRtmpStreaming({
    bool resetTracking = true,
    bool fromDispose = false,
  }) async {
    if (resetTracking) {
      _clearActiveStreamTracking();
    }

    await RtmpService.instance.stopStream();

    if (!fromDispose && mounted) {
      setState(() {
        _isStreamMode = false;
      });
    } else {
      _isStreamMode = false;
    }
  }

  Widget _buildStreamWidget() {
    return CameraStreamView(
      supportsMobileRtmp: _supportsMobileRtmp,
      controller: _controller,
      connectionUrl: widget.connectionUrl,
    );
  }

  Future<void> _suspendRtmpStreamForLifecycle() async {
    // flutter_rtmp_publisher handles lifecycle internally or we can't control it.
    if (_supportsMobileRtmp) {
      return;
    }

    if (Platform.isWindows && _activeStreamUrl != null && _isStreamMode) {
      _pendingRtmpResume = true;
      NativeCameraService().stop();
      if (mounted) {
        setState(() {});
      }
    }
  }

  Future<void> _resumeRtmpStreamIfNeeded() async {
    if (!_pendingRtmpResume) {
      return;
    }

    if (_supportsMobileRtmp) {
      return;
    }

    if (_activeStreamUrl != null && Platform.isWindows) {
      try {
        WakelockPlus.enable();
        NativeCameraService().startStream(_activeStreamUrl!);
        if (mounted) {
          setState(() {
            _isStreamMode = true;
          });
        }
      } catch (e) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text('Failed to resume stream: $e')),
          );
        }
      } finally {
        _pendingRtmpResume = false;
      }
    }
  }

  void _clearActiveStreamTracking() {
    _activeStreamUrl = null;
    _pendingRtmpResume = false;
  }

  Future<void> _connectToUrl(String url) async {
    WakelockPlus.enable();
    
    // Ensure complete cleanup of previous sessions
    await _releaseLocalCameraResources();
    await _stopMobileRtmpStreaming(resetTracking: true);

    String cleanUrl = url.trim();

    if (cleanUrl.startsWith('rtmp://')) {
      _activeStreamUrl = cleanUrl;
      _pendingRtmpResume = false;

      if (Platform.isAndroid) {
        // Connect to Raw Socket Server
        try {
          Uri uri = Uri.parse(cleanUrl);
          String ip = uri.host;
          RawSocketService.instance.connect(ip);
        } catch (e) {
          debugPrint('Error connecting to socket: $e');
        }

        // Start the stream here, after the screen has loaded
        await _startMobileRtmpStreaming(cleanUrl);
      } else if (Platform.isWindows) {
        // Just start the stream player
        NativeCameraService().startStream(cleanUrl);
        if (!mounted) return;
        setState(() {
          _isStreamMode = true;
        });
      } else {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('RTMP not supported on this platform'),
            ),
          );
        }
      }
      return;
    }
  }

  Future<void> _setDefaultPath() async {
    final dir = await getApplicationDocumentsDirectory();
    if (!mounted) return;
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
        // Yield to event loop to ensure scheduler is idle before calling native code
        // This prevents the "SchedulerPhase.idle" assertion error
        WidgetsBinding.instance.addPostFrameCallback((_) async {
           await Future.delayed(Duration.zero);
           NativeCameraService().selectCamera(_selectedCameraIndex);
        });
      }
      return;
    }

    // On Android, if we have a connectionUrl, we still need to init the camera
    // because we want to show the preview (even if not transmitting).
    // if (widget.connectionUrl != null) return;

    // If we are streaming via RTMP on Android, we should NOT initialize the local camera plugin
    // because HaishinKit will handle the camera.
    if (Platform.isAndroid &&
        widget.connectionUrl != null &&
        widget.connectionUrl!.startsWith('rtmp://')) {
      return;
    }

    await _loadCameras();
    if (_cameras.isNotEmpty) {
      _selectedCameraIndex = 0;
      await _startLocalCamera();
    }
  }

  Future<void> _loadCameras() async {
    try {
      _cameras = await c.availableCameras();
    } catch (e) {
      debugPrint('Error loading cameras: $e');
    }
  }

  Future<void> _showMobileConnectionDialog() async {
    // Ensure we have network info
    if (_serverIp.isEmpty) {
      try {
        final interfaces = await RawSocketService.instance.getNetworkInterfaces();
        if (!mounted) return;
        if (interfaces.isNotEmpty) {
          setState(() {
            _availableInterfaces = interfaces;
            _serverIp = interfaces[0].ip;
          });
        }
      } catch (e) {
        debugPrint('Error getting interfaces: $e');
      }
    }

    if (!mounted) return;

    _isQrDialogVisible = true;

    // Listen for new streams while dialog is open
    final streamSubscription = MediaServerService.instance.onStreamStarted.listen((path) {
      if (path == 'live/stream' && mounted && _isQrDialogVisible) {
         // Close dialog and connect
         Navigator.of(context).pop();
         _connectToStream('rtmp://localhost/live/stream');
      }
    });

    await showDialog(
      context: context,
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setStateDialog) {
            // Check if stream is active and auto-close if so
            if (MediaServerService.instance.isStreamActive('live/stream')) {
               // Use a post-frame callback to close the dialog safely
               WidgetsBinding.instance.addPostFrameCallback((_) {
                  if (mounted && _isQrDialogVisible) {
                     Navigator.of(context).pop();
                     // Trigger connection logic
                     _connectToStream('rtmp://localhost/live/stream');
                  }
               });
            }
            
            return AlertDialog(
              title: const Text('Connect Mobile Camera'),
              content: SizedBox(
                width: 300,
                height: 450,
                child: SingleChildScrollView(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      // Server IP Selection
                      if (_availableInterfaces.isNotEmpty)
                        Padding(
                          padding: const EdgeInsets.only(bottom: 16.0),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              const Text('Server Interface:', style: TextStyle(color: Colors.grey, fontSize: 12)),
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
                                    // Update parent state
                                    this.setState(() {
                                      _serverIp = val;
                                    });
                                    // Update dialog state
                                    setStateDialog(() {});
                                  }
                                },
                              ),
                            ],
                          ),
                        ),

                      // QR Code
                      if (_serverIp.isNotEmpty) ...[
                        Container(
                          color: Colors.white,
                          padding: const EdgeInsets.all(16),
                          child: QrImageView(
                            data: 'rtmp://$_serverIp/live/stream',
                            version: QrVersions.auto,
                            size: 200.0,
                          ),
                        ),
                        const SizedBox(height: 16),
                        Text(
                          'Scan with Kaptchi mobile app',
                          style: TextStyle(fontSize: 16, color: Colors.grey[600]),
                          textAlign: TextAlign.center,
                        ),
                        const SizedBox(height: 16),
                        
                        // Media Server Status
                        if (MediaServerService.instance.isRunning)
                          const Row(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: [
                              Icon(Icons.check_circle, color: Colors.green, size: 16),
                              SizedBox(width: 8),
                              Text('Media Server Running', style: TextStyle(color: Colors.green, fontSize: 12)),
                            ],
                          )
                        else
                          const Text(
                            'Media Server Stopped',
                            style: TextStyle(color: Colors.red, fontSize: 12),
                            textAlign: TextAlign.center,
                          ),
                      ],
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
            );
          }
        );
      },
    );

    _isQrDialogVisible = false;
    streamSubscription.cancel();
  }

  Future<void> _switchCamera() async {
    if (Platform.isWindows) {
      // Fetch available cameras for the dropdown
      List<c.CameraDescription> cameras = [];
      try {
        cameras = await c.availableCameras();
      } catch (e) {
        debugPrint('Error getting cameras: $e');
      }

      if (!mounted) return;

      showModalBottomSheet(
        context: context,
        builder: (context) => Container(
          padding: const EdgeInsets.all(16),
          child: SingleChildScrollView(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Text(
                  'Select Video Source',
                  style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold),
                ),
                const SizedBox(height: 16),

                // Local Cameras
                if (cameras.isNotEmpty) ...[
                  const Align(
                    alignment: Alignment.centerLeft,
                    child: Text(
                      'Local Cameras',
                      style: TextStyle(
                        fontWeight: FontWeight.bold,
                        color: Colors.grey,
                      ),
                    ),
                  ),
                  ...cameras.asMap().entries.map((entry) {
                    final index = entry.key;
                    final camera = entry.value;
                    // Clean up camera name by removing ID part in angle brackets <...>
                    final cleanName = camera.name
                        .replaceAll(RegExp(r'<[^>]*>'), '')
                        .trim();

                    return ListTile(
                      leading: const Icon(Icons.camera),
                      title: Text(cleanName),
                      onTap: () async {
                        Navigator.pop(context);
                        
                        // Stop any active stream before switching
                        NativeCameraService().stop();
                        
                        setState(() {
                          _isStreamMode = false;
                          _isDigitalZoomOverride =
                              true; // Enable by default for local cameras
                          _lockedPhoneZoom = 1.0;
                          _currentZoom = 1.0;
                          _viewOffset = Offset.zero;
                        });

                        // Fix for swapped cameras when exactly 2 are present
                        // The user reported that selecting one opens the other.
                        int targetIndex = index;
                        if (cameras.length == 2) {
                           targetIndex = (index == 0) ? 1 : 0;
                        }

                        NativeCameraService().selectCamera(targetIndex);
                      },
                    );
                  }),
                  const Divider(),
                ],

                const Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    'Remote Streams',
                    style: TextStyle(
                      fontWeight: FontWeight.bold,
                      color: Colors.grey,
                    ),
                  ),
                ),
                ListTile(
                  leading: const Icon(Icons.phone_android),
                  title: const Text('Mobile App'),
                  subtitle: const Text('Connect via QR Code'),
                  onTap: () {
                    Navigator.pop(context);
                    _showMobileConnectionDialog();
                  },
                ),
              ],
            ),
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
        NativeCameraService().setResolution(4096, 2160);
      } else {
        NativeCameraService().setResolution(1920, 1080);
      }
    } else {
      await _startLocalCamera();
    }
  }

  void _sendZoomCommand(double zoom) {
    debugPrint('Sending zoom command: $zoom');
    if (!Platform.isWindows) return;
    
    // Direct broadcast via RawSocketService (Host mode)
    RawSocketService.instance.send('control', {
        'command': 'zoom',
        'data': {'level': zoom}
    });
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

    // 3. Update Image Processing Service (Streamed)
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
    // Capture state before async operations or state changes
    final wasStreamMode = _isStreamMode;

    WidgetsBinding.instance.removeObserver(this);
    WakelockPlus.disable();
    _controller?.dispose();
    unawaited(_stopMobileRtmpStreaming(fromDispose: true));
    _transformationController.dispose();
    _clearActiveStreamTracking();
    _frameTimer?.cancel();
    _mediaServerSubscription?.cancel();
    _streamStoppedSubscription?.cancel();

    if (Platform.isWindows && wasStreamMode) {
      NativeCameraService().stop();
    }

    super.dispose();
  }

  Future<void> _startLocalCamera() async {
    if (Platform.isWindows) {
      // On Windows, we use NativeCameraService via NativeCameraView.
      // We do NOT initialize package:camera controller to avoid conflicts.
      if (mounted) {
        setState(() {
          _isInitialized = true;
        });
      }
      return;
    } else {
      // Android / iOS
      if (_controller != null) {
        await _controller!.dispose();
      }
      if (_cameras.isEmpty) return;

      if (_selectedCameraIndex < 0 || _selectedCameraIndex >= _cameras.length) {
        _selectedCameraIndex = 0;
      }

      final camera = _cameras[_selectedCameraIndex];
      final resolution = _isHighQuality
          ? c.ResolutionPreset.max
          : c.ResolutionPreset.medium;

      _controller = c.CameraController(
        camera,
        resolution,
        enableAudio: false,
        imageFormatGroup: Platform.isAndroid
            ? c.ImageFormatGroup.jpeg
            : c.ImageFormatGroup.bgra8888,
      );

      try {
        await _controller!.initialize();

        // Get zoom levels
        _minAvailableZoom = await _controller!.getMinZoomLevel();

        if (!mounted) return;
        setState(() {
          _isInitialized = true;
        });
      } catch (e) {
        debugPrint('Error starting camera: $e');
        if (mounted) {
          setState(() {
            _isInitialized = false;
          });
          ScaffoldMessenger.of(
            context,
          ).showSnackBar(SnackBar(content: Text('Error starting camera: $e')));
        }
      }
    }
  }

  Future<void> _captureFrame() async {
    if (!Platform.isWindows) return;

    try {
      Uint8List? finalBytes;
      int finalWidth = 0;
      int finalHeight = 0;

      if (_isStreamMode) {
        // Use NativeCameraService for Windows RTMP capture
        final service = NativeCameraService();
        final width = service.getFrameWidth();
        final height = service.getFrameHeight();
        final bytes = service.getFrameData();

        if (bytes != null && width > 0 && height > 0) {
          final image = img.Image.fromBytes(
            width: width,
            height: height,
            bytes: bytes.buffer,
            numChannels: 4,
            order: img.ChannelOrder.rgba,
          );
          finalBytes = img.encodePng(image);
          finalWidth = width;
          finalHeight = height;
        } else {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('Failed to capture frame')),
          );
          return;
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
          cropX = 0;
          cropY = 0;
          cropW = width;
          cropH = height;
        }

        final croppedImage = img.copyCrop(
          image,
          x: cropX,
          y: cropY,
          width: cropW,
          height: cropH,
        );
        finalBytes = img.encodePng(croppedImage);
        finalWidth = cropW;
        finalHeight = cropH;
      }

      if (finalBytes != null) {
        if (!mounted) return;
        setState(() {
          _capturedImages.add((
            bytes: finalBytes!,
            width: finalWidth,
            height: finalHeight,
          ));
        });

        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Captured image ${_capturedImages.length}')),
        );
      }
    } catch (e) {
      debugPrint('Error capturing frame: $e');
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Error capturing frame: $e')));
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
            pageFormat: PdfPageFormat(
              item.width.toDouble(),
              item.height.toDouble(),
            ),
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
        fileName =
            "Capture_${now.year}-${now.month.toString().padLeft(2, '0')}-${now.day.toString().padLeft(2, '0')}_${now.hour.toString().padLeft(2, '0')}-${now.minute.toString().padLeft(2, '0')}";
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

      if (!mounted) return;
      setState(() {
        _capturedImages.clear();
        _pdfNameController.clear();
      });

      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Saved PDF to ${file.path}')));
    } catch (e) {
      debugPrint('Error exporting PDF: $e');
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Error exporting PDF: $e')));
    }
  }


  Future<void> _scanQrCode() async {
    // Dispose local camera before scanning to avoid conflicts
    if (_controller != null) {
      await _controller!.dispose();
      _controller = null;
    }
    if (!mounted) return;
    setState(() {
      _isInitialized = false;
    });

    // Hold exclusive camera ownership for the scanner.
    await CameraOwnerCoordinator.instance.acquire('scanner');
    var scannerLeaseHeld = true;

    // Create a controller to manage the camera lifecycle explicitly
    final MobileScannerController scannerController = MobileScannerController(
      detectionSpeed: DetectionSpeed.noDuplicates,
      returnImage: false,
    );

    Object? result;
    try {
      result = await Navigator.push(
        context,
        MaterialPageRoute(
          builder: (context) => Scaffold(
            appBar: AppBar(title: const Text('Scan QR Code')),
            body: MobileScanner(
              controller: scannerController,
              onDetect: (capture) {
                final List<Barcode> barcodes = capture.barcodes;
                for (final barcode in barcodes) {
                  if (barcode.rawValue != null) {
                    scannerController.stop();
                    Navigator.pop(context, barcode.rawValue);
                    return;
                  }
                }
              },
            ),
          ),
        ),
      );
    } finally {
      scannerController.dispose();
      if (scannerLeaseHeld) {
        CameraOwnerCoordinator.instance.release('scanner');
        scannerLeaseHeld = false;
      }
    }

    if (!mounted) return;

    if (result != null && result is String) {
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(SnackBar(content: Text('Connecting to $result...')));
      }

      // Ensure local controller is definitely cleared
      if (!mounted) return;
      setState(() {
        _controller = null;
        _isInitialized = false;
      });

      if (!mounted) return;

      // Enable Wakelock to prevent screen from turning off during streaming
      WakelockPlus.enable();

      if (result.startsWith('rtmp://')) {
        _activeStreamUrl = result;
        _pendingRtmpResume = false;

        if (Platform.isAndroid) {
          // Dispose local camera to free resources before starting HaishinKit
          if (_controller != null) {
            await _controller!.dispose();
            _controller = null;
          }

          // Wait for camera resources to be fully released by the OS
          await Future.delayed(const Duration(milliseconds: 500));

          if (!mounted) return;

          // Navigate to the "Transmitting" screen using pushReplacement to avoid keeping the scanner in stack
          // The new screen will handle starting the RTMP stream in its initialization
          Navigator.pushReplacement(
            context,
            MaterialPageRoute(
              builder: (context) => CameraScreen(connectionUrl: result as String),
            ),
          );
          return;
        }

        if (Platform.isWindows) {
          NativeCameraService().startStream(result);
          if (mounted) {
            setState(() {
              _isStreamMode = true;
            });
          }
          return;
        }

        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('RTMP not supported on this platform'),
            ),
          );
        }
      } else {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Unsupported protocol. Only RTMP is supported.'),
            ),
          );
        }
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    if (widget.connectionUrl != null) {
      return Scaffold(
        appBar: AppBar(title: const Text('Transmitting')),
        body: Stack(
          children: [
            // Video Preview
            Positioned.fill(child: _buildStreamWidget()),

            // Overlay Controls
            Positioned(
              bottom: 40,
              left: 0,
              right: 0,
              child: Center(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const SizedBox(height: 20),
                    ElevatedButton.icon(
                      onPressed: () async {
                        WakelockPlus.disable();
                        await _stopMobileRtmpStreaming();
                        if (Platform.isAndroid) {
                          await RtmpService.instance.stopStream();
                        }
                        _clearActiveStreamTracking();
                        if (!context.mounted) return;
                        Navigator.pop(context);
                      },
                      icon: const Icon(Icons.stop),
                      label: const Text('Stop Transmission'),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.red,
                        foregroundColor: Colors.white,
                        padding: const EdgeInsets.symmetric(
                          horizontal: 32,
                          vertical: 16,
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
            icon: const Icon(Icons.home),
            tooltip: 'Back to Home',
            onPressed: () {
              // Just pop, let dispose handle cleanup to avoid state races
              if (Navigator.canPop(context)) {
                Navigator.pop(context);
              }
            },
          ),
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
                                  double scaleFactor = event.scrollDelta.dy > 0
                                      ? 0.9
                                      : 1.1;

                                  // Calculate min zoom: 1.0 normally, or locked zoom if override is on
                                  double minZoom = _isDigitalZoomOverride
                                      ? _lockedPhoneZoom
                                      : 1.0;
                                  double newTotalZoom =
                                      (_currentZoom * scaleFactor).clamp(
                                        minZoom,
                                        100.0,
                                      );

                                  // Calculate visual scales for offset adjustment
                                  double oldVisualZoom;
                                  double newVisualZoom;

                                  if (_isDigitalZoomOverride) {
                                    oldVisualZoom =
                                        _currentZoom / _lockedPhoneZoom;
                                    newVisualZoom =
                                        newTotalZoom / _lockedPhoneZoom;
                                  } else {
                                    oldVisualZoom = _currentZoom > _phoneMaxZoom
                                        ? _currentZoom / _phoneMaxZoom
                                        : 1.0;
                                    newVisualZoom = newTotalZoom > _phoneMaxZoom
                                        ? newTotalZoom / _phoneMaxZoom
                                        : 1.0;
                                  }

                                  double visualScaleDelta =
                                      newVisualZoom / oldVisualZoom;

                                  setState(() {
                                    _currentZoom = newTotalZoom;

                                    if (newVisualZoom <= 1.0) {
                                      _viewOffset = Offset.zero;
                                    } else {
                                      // Apply Zoom around cursor position
                                      // Convert cursor position to be relative to center
                                      Offset center = Offset(
                                        constraints.maxWidth / 2,
                                        constraints.maxHeight / 2,
                                      );
                                      Offset focalPointFromCenter =
                                          event.localPosition - center;

                                      // Adjust offset to keep focal point stable during zoom
                                      _viewOffset -=
                                          (focalPointFromCenter - _viewOffset) *
                                          (visualScaleDelta - 1);

                                      // Clamp Offset
                                      double maxDx =
                                          (constraints.maxWidth *
                                                  newVisualZoom -
                                              constraints.maxWidth) /
                                          2;
                                      double maxDy =
                                          (constraints.maxHeight *
                                                  newVisualZoom -
                                              constraints.maxHeight) /
                                          2;
                                      _viewOffset = Offset(
                                        _viewOffset.dx.clamp(-maxDx, maxDx),
                                        _viewOffset.dy.clamp(-maxDy, maxDy),
                                      );
                                    }

                                    // Send command to phone (clamped)
                                    if (!_isDigitalZoomOverride &&
                                        _isStreamMode) {
                                      _sendZoomCommand(
                                        _currentZoom.clamp(1.0, _phoneMaxZoom),
                                      );
                                    } else if (_isDigitalZoomOverride &&
                                        _isStreamMode) {
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
                                  double scaleDelta =
                                      details.scale / _lastScale;
                                  _lastScale = details.scale;

                                  // Calculate min zoom: 1.0 normally, or locked zoom if override is on
                                  double minZoom = _isDigitalZoomOverride
                                      ? _lockedPhoneZoom
                                      : 1.0;
                                  double newTotalZoom =
                                      (_currentZoom * scaleDelta).clamp(
                                        minZoom,
                                        100.0,
                                      );

                                  // Calculate visual scales for offset adjustment
                                  double oldVisualZoom;
                                  double newVisualZoom;

                                  if (_isDigitalZoomOverride) {
                                    oldVisualZoom =
                                        _currentZoom / _lockedPhoneZoom;
                                    newVisualZoom =
                                        newTotalZoom / _lockedPhoneZoom;
                                  } else {
                                    oldVisualZoom = _currentZoom > _phoneMaxZoom
                                        ? _currentZoom / _phoneMaxZoom
                                        : 1.0;
                                    newVisualZoom = newTotalZoom > _phoneMaxZoom
                                        ? newTotalZoom / _phoneMaxZoom
                                        : 1.0;
                                  }

                                  double visualScaleDelta =
                                      newVisualZoom / oldVisualZoom;

                                  // 2. Calculate Pan
                                  Offset focalPointDelta =
                                      details.localFocalPoint -
                                      _lastFocalPoint!;
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
                                      Offset center = Offset(
                                        constraints.maxWidth / 2,
                                        constraints.maxHeight / 2,
                                      );
                                      Offset focalPointFromCenter =
                                          details.localFocalPoint - center;

                                      // Adjust offset to keep focal point stable during zoom
                                      _viewOffset -=
                                          (focalPointFromCenter - _viewOffset) *
                                          (visualScaleDelta - 1);

                                      // Clamp Offset
                                      double maxDx =
                                          (constraints.maxWidth *
                                                  newVisualZoom -
                                              constraints.maxWidth) /
                                          2;
                                      double maxDy =
                                          (constraints.maxHeight *
                                                  newVisualZoom -
                                              constraints.maxHeight) /
                                          2;
                                      _viewOffset = Offset(
                                        _viewOffset.dx.clamp(-maxDx, maxDx),
                                        _viewOffset.dy.clamp(-maxDy, maxDy),
                                      );
                                    }

                                    // Send command to phone (clamped)
                                    if (!_isDigitalZoomOverride &&
                                        _isStreamMode) {
                                      _sendZoomCommand(
                                        _currentZoom.clamp(1.0, _phoneMaxZoom),
                                      );
                                    } else if (_isDigitalZoomOverride &&
                                        _isStreamMode) {
                                      // Keep phone locked at the override level
                                      _sendZoomCommand(_lockedPhoneZoom);
                                    }
                                  });
                                },
                                child: ClipRect(
                                  child: LayoutBuilder(
                                    builder: (context, _) {
                                      // Calculate effective zooms based on override mode
                                      double effectivePhoneZoom =
                                          _isDigitalZoomOverride
                                          ? _lockedPhoneZoom
                                          : _currentZoom.clamp(
                                              1.0,
                                              _phoneMaxZoom,
                                            );
                                      double effectiveVisualZoom =
                                          _currentZoom / effectivePhoneZoom;

                                      return Transform(
                                        transform: Matrix4.identity()
                                          ..translateByVector3(
                                            vm.Vector3(
                                              _viewOffset.dx,
                                              _viewOffset.dy,
                                              0,
                                            ),
                                          )
                                          ..scaleByVector3(
                                            vm.Vector3(
                                              effectiveVisualZoom,
                                              effectiveVisualZoom,
                                              1,
                                            ),
                                          ),
                                        alignment: Alignment.center,
                                        child: _isStreamMode
                                            ? Stack(
                                                children: [
                                                  // Hidden RepaintBoundary for capture
                                                  Opacity(
                                                    opacity: 1.0,
                                                    child: RepaintBoundary(
                                                      key: _videoViewKey,
                                                      child:
                                                          _buildStreamWidget(),
                                                    ),
                                                  ),
                                                  // Processed overlay
                                                  ValueListenableBuilder<
                                                    Uint8List?
                                                  >(
                                                    valueListenable:
                                                        _processedImageNotifier,
                                                    builder:
                                                        (
                                                          context,
                                                          processedImage,
                                                          child,
                                                        ) {
                                                          bool
                                                          hasActiveFilters =
                                                              _filters.any(
                                                                (f) =>
                                                                    f.isActive,
                                                              );
                                                          if (processedImage !=
                                                                  null &&
                                                              hasActiveFilters) {
                                                            return Opacity(
                                                              opacity: 0.995,
                                                              child: Image.memory(
                                                                processedImage,
                                                                gaplessPlayback:
                                                                    true,
                                                                fit: BoxFit
                                                                    .contain,
                                                                width: double
                                                                    .infinity,
                                                                height: double
                                                                    .infinity,
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
                                    },
                                  ),
                                ),
                              ),
                            );
                          },
                        )
                      else if (_isInitialized)
                        InteractiveViewer(
                          transformationController: _transformationController,
                          minScale: 1.0,
                          maxScale: 50.0,
                          child: ValueListenableBuilder<Uint8List?>(
                            valueListenable: _processedImageNotifier,
                            builder: (context, processedImage, child) {
                              bool hasActiveFilters = _filters.any(
                                (f) => f.isActive,
                              );
                              if (processedImage != null && hasActiveFilters) {
                                return Image.memory(
                                  processedImage,
                                  gaplessPlayback: true,
                                  fit: BoxFit.contain,
                                );
                              }
                              if (_controller != null) {
                                return c.CameraPreview(_controller!);
                              }
                              return const SizedBox.shrink();
                            },
                          ),
                        )
                      else if (_isStreamMode)
                        InteractiveViewer(
                          transformationController: _transformationController,
                          minScale: 1.0,
                          maxScale: 50.0,
                          child: _buildStreamWidget(),
                        )
                      else
                        Center(
                          child: Column(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: [
                              if (_cameras.isEmpty && !Platform.isWindows)
                                const Text('No cameras found')
                              else if (_controller == null &&
                                  !Platform.isWindows)
                                const CircularProgressIndicator()
                              else
                                Column(
                                  children: [
                                    const Icon(
                                      Icons.error_outline,
                                      size: 48,
                                      color: Colors.red,
                                    ),
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
                              child: const Icon(
                                Icons.switch_camera,
                                color: Colors.white,
                              ),
                            ),
                            // Add the Pan/Digital Zoom Override Button
                            if (_isStreamMode)
                              FloatingActionButton(
                                heroTag: 'pan_mode',
                                onPressed: () {
                                  setState(() {
                                    _isDigitalZoomOverride =
                                        !_isDigitalZoomOverride;
                                    if (_isDigitalZoomOverride) {
                                      // Lock the phone zoom at the current level
                                      _lockedPhoneZoom = _currentZoom.clamp(
                                        1.0,
                                        _phoneMaxZoom,
                                      );
                                      // We don't reset _currentZoom, we just continue from here
                                      // Ensure we send the lock command once
                                      _sendZoomCommand(_lockedPhoneZoom);
                                    } else {
                                      // Restore phone zoom control
                                      _sendZoomCommand(
                                        _currentZoom.clamp(1.0, _phoneMaxZoom),
                                      );
                                    }
                                  });
                                },
                                backgroundColor: _isDigitalZoomOverride
                                    ? Colors.blue
                                    : Colors.black54,
                                child: Icon(
                                  _isDigitalZoomOverride
                                      ? Icons.lock
                                      : Icons.lock_open,
                                  color: Colors.white,
                                ),
                              ),
                            // Removed filter cycle button
                            FloatingActionButton(
                              heroTag: 'max_quality',
                              onPressed: _toggleQuality,
                              backgroundColor: _isHighQuality
                                  ? Colors.blue
                                  : Colors.black54,
                              child: const Icon(
                                Icons.high_quality,
                                color: Colors.white,
                              ),
                            ),
                            FloatingActionButton(
                              heroTag: 'capture_frame',
                              onPressed: _captureFrame,
                              backgroundColor: Colors.red,
                              child: const Icon(
                                Icons.camera_alt,
                                color: Colors.white,
                              ),
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
          FilterSidebar(
            isOpen: _isLeftSidebarOpen,
            onClose: () => setState(() => _isLeftSidebarOpen = false),
            filters: _filters,
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
            onFilterToggle: (index, value) {
              setState(() {
                _filters[index].isActive = value;
                _updateFilters();
              });
            },
          ),

          // Right Sidebar (Captured Images)
          GallerySidebar(
            isOpen: _isSidebarOpen,
            isFullScreen: _isGalleryFullScreen,
            capturedImages: _capturedImages,
            onClose: () => setState(() => _isSidebarOpen = false),
            onToggleFullScreen: () =>
                setState(() => _isGalleryFullScreen = !_isGalleryFullScreen),
            onOpenEditor: _openEditor,
            onDeleteImage: (index) {
              setState(() {
                _capturedImages.removeAt(index);
              });
            },
            onPageChanged: (index) {
              setState(() {
                _currentGalleryIndex = index;
              });
            },
            pdfNameController: _pdfNameController,
            pdfPathController: _pdfPathController,
            onExportPdf: _exportPdf,
            onSelectDirectory: () async {
              String? selectedDirectory = await FilePicker.platform
                  .getDirectoryPath();
              if (selectedDirectory != null) {
                setState(() {
                  _pdfPathController.text = selectedDirectory;
                });
              }
            },
          ),
        ],
      ),
    );
  }
}
