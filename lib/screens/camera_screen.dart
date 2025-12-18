import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:camera/camera.dart' as c;
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:image/image.dart' as img;
import 'package:file_picker/file_picker.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

import 'package:wakelock_plus/wakelock_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'image_editor_screen.dart';
import 'crop_screen.dart';
import '../services/media_server_service.dart';
import '../services/image_processing_service.dart';
import '../widgets/native_camera_view.dart';
import '../services/native_camera_service.dart';
import '../services/rtmp_service.dart';
import '../services/raw_socket_service.dart';
import '../widgets/camera_sidebars.dart';
import '../widgets/camera_stream_view.dart';
import '../widgets/resizable_overlay.dart';
import '../services/gallery_service.dart';
import '../services/filters_service.dart';
import '../widgets/mobile_connection_dialog.dart';
import '../widgets/video_source_sheet.dart';
import '../widgets/zoomable_stream_view.dart';

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
    with WidgetsBindingObserver, SingleTickerProviderStateMixin {
  final _transformationController = TransformationController();

  c.CameraController? _controller;
  List<c.CameraDescription> _cameras = [];
  int _selectedCameraIndex = -1;
  bool _isInitialized = false;

  // Remote / Stream Mode State
  bool _isStreamMode = false;

  final GlobalKey _videoViewKey = GlobalKey();
  final GlobalKey _zoomedViewKey =
      GlobalKey(); // For screenshot capture of zoomed content

  // Image processing state
  final ValueNotifier<Uint8List?> _processedImageNotifier = ValueNotifier(null);

  // Zoom state
  // Zoom state
  double _currentZoom = 1.0;
  double _phoneMaxZoom = 10.0;
  bool _isDigitalZoomOverride = false;
  double _lockedPhoneZoom = 1.0;
  Offset _viewOffset = Offset.zero;

  // Overlay State (Feature 11)
  Uint8List? _overlayImageBytes;
  Offset _overlayPosition = Offset.zero;
  double _overlayWidth = 200; // Display width (resizable)
  double _overlayHeight = 150; // Display height (resizable)

  // Captured images ref (Now using GalleryService)
  List<({Uint8List bytes, int width, int height})> get _capturedImages =>
      GalleryService.instance.images;
  int _currentGalleryIndex = 0;
  String? _activeStreamUrl;
  bool _pendingRtmpResume = false;

  // Sidebar state
  bool _isSidebarOpen = false;
  bool _isLeftSidebarOpen = false;
  bool _isGalleryFullScreen = false;
  final TextEditingController _pdfNameController = TextEditingController();
  final TextEditingController _pdfPathController = TextEditingController();

  // Flash Animation State
  late AnimationController _flashController;
  late Animation<double> _flashAnimation;

  // Filter state managed by FiltersService

  void _onFiltersChanged() {
    if (mounted) {
      setState(() {
        _updateFilters();
      });
    }
  }

  Future<void> _loadPdfPath() async {
    final prefs = await SharedPreferences.getInstance();
    final String? lastPath = prefs.getString('last_pdf_path');

    String initialPath;
    if (lastPath != null) {
      initialPath = lastPath;
    } else {
      final dir = await getApplicationDocumentsDirectory();
      initialPath = dir.path;
    }

    if (mounted) {
      setState(() {
        _pdfPathController.text = initialPath;
      });
    }
  }

  StreamSubscription? _mediaServerSubscription;
  StreamSubscription? _streamStoppedSubscription;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);

    // Set default max zoom based on platform
    // Windows cameras usually don't support optical zoom commands, so we limit to 1.0
    // to prevent "empty" zoom range where nothing happens visually.
    // If connecting to a remote stream later, this can be updated.
    _phoneMaxZoom = Platform.isWindows ? 1.0 : 10.0;

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
      _mediaServerSubscription = MediaServerService.instance.onStreamStarted
          .listen((path) async {
            debugPrint('Stream started: $path');
            if (mounted) {
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
      _streamStoppedSubscription = MediaServerService.instance.onStreamStopped
          .listen((path) {
            if (path == 'live/stream' && mounted) {
              // If we are currently viewing this stream, go back
              if (_isStreamMode &&
                  (_activeStreamUrl?.contains(path) ?? false)) {
                ScaffoldMessenger.of(context).showSnackBar(
                  SnackBar(
                    content: Text(
                      AppLocalizations.of(context)!.streamDisconnected,
                    ),
                  ),
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
    FiltersService.instance.loadFilters();
    FiltersService.instance.addListener(_onFiltersChanged);
    _loadPdfPath();

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

    // Listen for gallery changes to update UI immediately
    GalleryService.instance.addListener(_onGalleryChange);

    // Initialize Flash Animation
    _flashController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 150),
    );
    _flashAnimation = Tween<double>(
      begin: 0.0,
      end: 1.0,
    ).animate(CurvedAnimation(parent: _flashController, curve: Curves.easeOut));
    _flashController.addStatusListener((status) {
      if (status == AnimationStatus.completed) {
        _flashController.reverse();
      }
    });
  }

  void _onGalleryChange() {
    if (mounted) setState(() {});
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

      // Assume remote stream supports zoom (e.g. Android phone)
      _phoneMaxZoom = 10.0;
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
          SnackBar(
            content: Text(AppLocalizations.of(context)!.permissionsRequired),
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
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              AppLocalizations.of(context)!.failedToStartStream(e.toString()),
            ),
          ),
        );
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
            SnackBar(
              content: Text(
                AppLocalizations.of(
                  context,
                )!.failedToResumeStream(e.toString()),
              ),
            ),
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
            SnackBar(
              content: Text(AppLocalizations.of(context)!.rtmpNotSupported),
            ),
          );
        }
      }
      return;
    }
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
        builder: (context) => VideoSourceSheet(
          cameras: cameras,
          onSelectCamera: (index) {
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

            NativeCameraService().selectCamera(index);
          },
          onSelectMobile: () {
            Navigator.pop(context);
            showDialog(
              context: context,
              builder: (context) => MobileConnectionDialog(
                onConnect: () {
                  // Usually called by the dialog when connection is successful
                  // But dialog doesn't know about `_connectToStream` of this parent.
                  // MobileConnectionDialog should just notify or we handle it here.
                  // Wait, MobileConnectionDialog in my implementation calls onConnect when stream starts.
                  // So I just need to:
                  Navigator.of(context).pop();
                  _connectToStream('rtmp://localhost/live/stream');
                },
              ),
            );
          },
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

  void _sendZoomCommand(double zoom) {
    debugPrint('Sending zoom command: $zoom');
    if (!Platform.isWindows) return;

    // Direct broadcast via RawSocketService (Host mode)
    RawSocketService.instance.send('control', {
      'command': 'zoom',
      'data': {'level': zoom},
    });
  }

  void _updateFilters() {
    // 1. Get active filters
    final activeFilters = FiltersService.instance.getActiveFilterIds();

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

    _mediaServerSubscription?.cancel();
    _streamStoppedSubscription?.cancel();

    if (Platform.isWindows && wasStreamMode) {
      NativeCameraService().stop();
    }

    FiltersService.instance.removeListener(_onFiltersChanged);
    GalleryService.instance.removeListener(_onGalleryChange);

    _flashController.dispose();
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
      const resolution = c.ResolutionPreset.max;

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
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text(
                AppLocalizations.of(context)!.errorStartingCamera(e.toString()),
              ),
            ),
          );
        }
      }
    }
  }

  Future<void> _captureFrame() async {
    if (!Platform.isWindows) return;

    // Trigger visual flash immediately
    _flashController.forward(from: 0.0);

    try {
      // Simple approach: Capture exactly what's displayed on screen
      // using RepaintBoundary screenshot
      final RenderRepaintBoundary? boundary =
          _zoomedViewKey.currentContext?.findRenderObject()
              as RenderRepaintBoundary?;

      if (boundary == null) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(
              content: Text(
                AppLocalizations.of(context)!.boundaryNotFoundError,
              ),
            ),
          );
        }
        return;
      }

      // Capture the widget as an image (pixel ratio 1.0 for 1:1 screen pixels)
      final ui.Image image = await boundary.toImage(pixelRatio: 1.0);
      final ByteData? byteData = await image.toByteData(
        format: ui.ImageByteFormat.png,
      );

      if (!mounted) return;

      if (byteData == null) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(AppLocalizations.of(context)!.captureFrameError),
          ),
        );
        return;
      }

      final Uint8List pngBytes = byteData.buffer.asUint8List();

      if (!mounted) return;

      GalleryService.instance.addImage(pngBytes, image.width, image.height);

      if (!mounted) return;
    } catch (e) {
      debugPrint('Error capturing frame: $e');
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            AppLocalizations.of(context)!.errorCapturingFrame(e.toString()),
          ),
        ),
      );
    }
  }

  Future<void> _quickDraw() async {
    // Capture and go directly to editor
    int preCount = GalleryService.instance.images.length;
    await _captureFrame();

    if (GalleryService.instance.images.length > preCount) {
      // Capture success
      // Open editor for the new image (last one)
      _currentGalleryIndex = GalleryService.instance.images.length - 1;
      await _openEditor();
    }
  }

  Future<void> _openEditor() async {
    if (_capturedImages.isEmpty) return;
    if (_currentGalleryIndex >= _capturedImages.length) {
      _currentGalleryIndex = _capturedImages.length - 1;
    }

    final item = _capturedImages[_currentGalleryIndex];
    final Uint8List? result = await Navigator.push(
      context,
      MaterialPageRoute(
        builder: (context) => ImageEditorScreen(imageBytes: item.bytes),
      ),
    );

    if (result != null) {
      final decoded = img.decodeImage(result);
      if (decoded != null) {
        // Use service method to replace image, preventing UnmodifiableListMixin error
        GalleryService.instance.replaceImage(
          _currentGalleryIndex,
          result,
          decoded.width,
          decoded.height,
        );
      }
    }
  }

  Future<void> _pickSaveDirectory() async {
    String? selectedDirectory = await FilePicker.platform.getDirectoryPath();
    if (selectedDirectory != null) {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('last_pdf_path', selectedDirectory);

      setState(() {
        _pdfPathController.text = selectedDirectory;
      });
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

      // Persist the LAST USED path
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('last_pdf_path', dirPath);

      if (!mounted) return;
      setState(() {
        GalleryService.instance.clear();
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

  Future<void> _cropImage(int index) async {
    final images = GalleryService.instance.images;
    if (images.isEmpty || index < 0 || index >= images.length) {
      debugPrint(
        '_cropImage: Invalid index $index, images.length=${images.length}',
      );
      return;
    }
    final image = images[index];
    final newBytes = await Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => CropScreen(imageBytes: image.bytes)),
    );

    if (newBytes != null) {
      final decoded = img.decodeImage(newBytes);
      if (decoded != null) {
        GalleryService.instance.replaceImage(
          index,
          newBytes,
          decoded.width,
          decoded.height,
        );
        // Update UI to show the cropped image immediately
        if (mounted) {
          setState(() {});
        }
      }
    }
  }

  void _useAsOverlay(int index) {
    final images = GalleryService.instance.images;
    if (images.isEmpty || index < 0 || index >= images.length) {
      debugPrint(
        '_useAsOverlay: Invalid index $index, images.length=${images.length}',
      );
      return;
    }
    final image = images[index];
    setState(() {
      _overlayImageBytes = image.bytes;
      // Set initial size (200px wide, height calculated from aspect ratio)
      _overlayWidth = 200;
      _overlayHeight = 200 * image.height / image.width;
      // Start at a safe position that's definitely on screen
      _overlayPosition = const Offset(100, 100);
      _isSidebarOpen = false; // Close sidebar to show overlay
    });
  }

  @override
  Widget build(BuildContext context) {
    if (widget.connectionUrl != null) {
      return Scaffold(
        appBar: AppBar(
          title: Text(AppLocalizations.of(context)!.transmissionTitle),
        ),
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
                      label: Text(
                        AppLocalizations.of(context)!.stopTransmission,
                      ),
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

    return Directionality(
      textDirection: TextDirection.ltr,
      child: Scaffold(
        appBar: AppBar(
          title: Text(AppLocalizations.of(context)!.cameraMode),
          leading: IconButton(
            icon: Icon(_isLeftSidebarOpen ? Icons.chevron_left : Icons.tune),
            tooltip: _isLeftSidebarOpen
                ? AppLocalizations.of(context)!.closeFilters
                : AppLocalizations.of(context)!.openFilters,
            onPressed: () {
              setState(() {
                _isLeftSidebarOpen = !_isLeftSidebarOpen;
              });
            },
          ),
          actions: [
            IconButton(
              icon: const Icon(Icons.home),
              tooltip: AppLocalizations.of(context)!.backToHome,
              onPressed: () {
                // Just pop, let dispose handle cleanup to avoid state races
                if (Navigator.canPop(context)) {
                  Navigator.pop(context);
                }
              },
            ),
            IconButton(
              icon: Icon(_isSidebarOpen ? Icons.chevron_right : Icons.list),
              tooltip: _isSidebarOpen
                  ? AppLocalizations.of(context)!.closeGallery
                  : AppLocalizations.of(context)!.openGallery,
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
                          RepaintBoundary(
                            key: _zoomedViewKey,
                            child: ZoomableStreamView(
                              currentZoom: _currentZoom,
                              viewOffset: _viewOffset,
                              isDigitalZoomOverride: _isDigitalZoomOverride,
                              lockedPhoneZoom: _lockedPhoneZoom,
                              isStreamMode: _isStreamMode,
                              phoneMaxZoom: _phoneMaxZoom,
                              onTransformChanged: (zoom, offset, viewportSize) {
                                setState(() {
                                  _currentZoom = zoom;
                                  _viewOffset = offset;
                                  // viewportSize is no longer needed - we use screenshot capture
                                });
                              },
                              onSendZoomCommand: _sendZoomCommand,
                              child: _isStreamMode
                                  ? Stack(
                                      children: [
                                        // Hidden RepaintBoundary for capture
                                        Opacity(
                                          opacity: 1.0,
                                          child: RepaintBoundary(
                                            key: _videoViewKey,
                                            child: _buildStreamWidget(),
                                          ),
                                        ),
                                        // Processed overlay
                                        ValueListenableBuilder<Uint8List?>(
                                          valueListenable:
                                              _processedImageNotifier,
                                          builder:
                                              (context, processedImage, child) {
                                                bool hasActiveFilters =
                                                    FiltersService
                                                        .instance
                                                        .filters
                                                        .any((f) => f.isActive);
                                                if (processedImage != null &&
                                                    hasActiveFilters) {
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
                                                // Overlay is now rendered from main Stack widget
                                                return const SizedBox.shrink();
                                              },
                                        ),
                                      ],
                                    )
                                  : const NativeCameraView(),
                            ),
                          )
                        else if (_isInitialized)
                          InteractiveViewer(
                            transformationController: _transformationController,
                            minScale: 1.0,
                            maxScale: 50.0,
                            child: ValueListenableBuilder<Uint8List?>(
                              valueListenable: _processedImageNotifier,
                              builder: (context, processedImage, child) {
                                bool hasActiveFilters = FiltersService
                                    .instance
                                    .filters
                                    .any((f) => f.isActive);
                                if (processedImage != null &&
                                    hasActiveFilters) {
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
                                  Text(
                                    AppLocalizations.of(
                                      context,
                                    )!.noCamerasFound,
                                  )
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
                                      Text(
                                        AppLocalizations.of(
                                          context,
                                        )!.cameraFailed,
                                      ),
                                      if (!Platform.isWindows)
                                        TextButton(
                                          onPressed: _switchCamera,
                                          child: Text(
                                            AppLocalizations.of(
                                              context,
                                            )!.tryNextCamera,
                                          ),
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
                          child: Directionality(
                            textDirection: TextDirection.ltr,
                            child: Row(
                              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                              children: [
                                FloatingActionButton(
                                  heroTag: 'switch_camera',
                                  onPressed: _switchCamera,
                                  backgroundColor: Colors.black54,
                                  tooltip: AppLocalizations.of(
                                    context,
                                  )!.switchCamera,
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
                                            _currentZoom.clamp(
                                              1.0,
                                              _phoneMaxZoom,
                                            ),
                                          );
                                        }
                                      });
                                    },
                                    backgroundColor: _isDigitalZoomOverride
                                        ? Colors.blue
                                        : Colors.black54,
                                    tooltip: _isDigitalZoomOverride
                                        ? AppLocalizations.of(
                                            context,
                                          )!.unlockZoom
                                        : AppLocalizations.of(
                                            context,
                                          )!.lockZoom,
                                    child: Icon(
                                      _isDigitalZoomOverride
                                          ? Icons.lock
                                          : Icons.lock_open,
                                      color: Colors.white,
                                    ),
                                  ),
                                FloatingActionButton(
                                  heroTag: 'quick_draw',
                                  onPressed: _quickDraw,
                                  backgroundColor: Colors.blue,
                                  tooltip: AppLocalizations.of(
                                    context,
                                  )!.quickDraw,
                                  child: const Icon(
                                    Icons.draw,
                                    color: Colors.white,
                                  ),
                                ),
                                FloatingActionButton(
                                  heroTag: 'capture_frame',
                                  onPressed: _captureFrame,
                                  backgroundColor: Colors.red,
                                  tooltip: AppLocalizations.of(
                                    context,
                                  )!.captureFrame,
                                  child: const Icon(
                                    Icons.camera_alt,
                                    color: Colors.white,
                                  ),
                                ),
                              ],
                            ),
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
              filters: FiltersService.instance.filters,
              filterGroups: FiltersService.instance.filterGroups,
              onReorder: FiltersService.instance.reorderFilters,
              onFilterToggle: FiltersService.instance.toggleFilter,
              onGroupToggle: FiltersService.instance.toggleGroup,
              onAddGroup: FiltersService.instance.addGroup,
              onEditGroup: FiltersService.instance.editGroup,
              onDeleteGroup: FiltersService.instance.deleteGroup,
            ),

            // Overlay Widget (Feature 11) - Resizable with handles
            if (_overlayImageBytes != null)
              ResizableOverlay(
                imageBytes: _overlayImageBytes!,
                initialPosition: _overlayPosition,
                initialWidth: _overlayWidth,
                initialHeight: _overlayHeight,
                onRemove: () {
                  setState(() {
                    _overlayImageBytes = null;
                  });
                },
                onChanged: (position, width, height) {
                  setState(() {
                    _overlayPosition = position;
                    _overlayWidth = width;
                    _overlayHeight = height;
                  });
                },
              ),

            // Right Sidebar (Captured Images)
            GallerySidebar(
              isOpen: _isSidebarOpen,
              isFullScreen: _isGalleryFullScreen,
              capturedImages: GalleryService.instance.images,
              currentIndex: _currentGalleryIndex,
              onClose: () => setState(() => _isSidebarOpen = false),
              onToggleFullScreen: () =>
                  setState(() => _isGalleryFullScreen = !_isGalleryFullScreen),
              onOpenEditor: _openEditor,
              onDeleteImage: (index) {
                // Delete from RAM service
                GalleryService.instance.removeImage(index);

                // Validate current index
                if (_currentGalleryIndex >=
                    GalleryService.instance.images.length) {
                  _currentGalleryIndex =
                      (GalleryService.instance.images.length - 1).clamp(
                        0,
                        100000,
                      );
                }
                if (GalleryService.instance.images.isEmpty) {
                  _currentGalleryIndex = 0;
                  // Optionally exit full screen if empty?
                  // _isGalleryFullScreen = false;
                }

                setState(() {});
              },
              onPageChanged: (index) {
                setState(() {
                  _currentGalleryIndex = index;
                });
              },
              pdfNameController: _pdfNameController,
              pdfPathController: _pdfPathController,
              onExportPdf: _exportPdf,
              onSelectDirectory: _pickSaveDirectory,
              onCropImage: _cropImage,
              onUseAsOverlay: _useAsOverlay,
            ),

            // Flash Overlay
            IgnorePointer(
              child: AnimatedBuilder(
                animation: _flashAnimation,
                builder: (context, child) {
                  return Opacity(
                    opacity: _flashAnimation.value,
                    child: Container(color: Colors.black),
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}
