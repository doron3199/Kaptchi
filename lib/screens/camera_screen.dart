import 'dart:async';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:camera/camera.dart' as c;
import '../services/document_service.dart';
import '../models/graph_node_info.dart';

import 'package:image/image.dart' as img;
import 'package:file_picker/file_picker.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

import 'package:wakelock_plus/wakelock_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'image_editor_screen.dart';
import 'edit_canvas_screen.dart';
import 'crop_screen.dart';
import '../services/media_server_service.dart';
import '../services/image_processing_service.dart';
import '../services/native_camera_service.dart';
import '../services/live_share_service.dart';
import '../services/rtmp_service.dart';
import '../services/raw_socket_service.dart';
import '../widgets/camera_sidebars.dart';
import '../widgets/camera_stream_view.dart';
import '../widgets/native_texture_view.dart';
import '../widgets/resizable_overlay.dart';
import '../services/gallery_service.dart';
import '../services/filters_service.dart';
import '../widgets/mobile_connection_dialog.dart';
import '../widgets/video_source_sheet.dart';
import '../widgets/graph_history_sparkline.dart';

class CameraScreen extends StatefulWidget {
  final String? connectionUrl;
  final int? initialCameraIndex;
  final String? initialStreamUrl;
  final bool isVddCapture;
  /// When set, immediately opens this local video file in whiteboard canvas mode
  final String? initialVideoFilePath;

  const CameraScreen({
    super.key,
    this.connectionUrl,
    this.initialCameraIndex,
    this.initialStreamUrl,
    this.isVddCapture = false,
    this.initialVideoFilePath,
  });

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen>
    with WidgetsBindingObserver, SingleTickerProviderStateMixin {
  final _transformationController = TransformationController();
  final _canvasTransformationController = TransformationController();

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

  // Live Perspective Crop State
  bool _isLiveCropActive = false;

  // Virtual Display Capture Mode State
  bool _isVddCaptureMode = false;
  bool _vddScrollForwardsToDisplay =
      false; // false = zoom (default), true = forward scroll

  // Whiteboard Canvas Mode State
  bool _isWhiteboardMode = false;
  bool _isCanvasViewMode = false;
  bool _isHighWhiteboardSensitivity = false;
  double? _canvasAspectRatio; // null = use default 16:9
  Timer? _canvasPollTimer;
  List<GraphHistoryTimelineEntry> _graphHistoryTimeline = const [];
  int _selectedGraphHistoryIndex = -1;
  int _graphHistoryPeakIndex = -1;
  bool _graphHistoryFollowLatest = true;
  bool _isDraggingGraphHistory = false;
  double _graphHistoryDragValue = 0.0;

  // Video file playback state
  bool _isVideoFileMode = false;
  double _videoProgress = 0.0;
  bool _isVideoPaused = false;
  bool _isVideoComplete = false;
  // When the user is dragging the seek slider, we show their chosen value
  // locally and ignore the polled progress until they release.
  bool _isSeekingVideo = false;
  double _seekDragValue = 0.0;
  // Notifier for canvas navigation state — updated by the poll timer without
  // a full setState rebuild (prevents live-view flicker).
  final _canvasNavNotifier = ValueNotifier<({int count, int active})>((
    count: 0,
    active: 0,
  ));

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

  void _startCanvasPollTimer() {
    _canvasPollTimer?.cancel();
    unawaited(_refreshGraphHistoryState());
    _canvasPollTimer = Timer.periodic(const Duration(milliseconds: 800), (_) async {
      if (!mounted || !Platform.isWindows) return;
      final svc = NativeCameraService();
      final count = svc.getSubCanvasCount();
      final activeVec = svc.getActiveSubCanvasIndex();
      // Convert raw vector index to spatial sorted position
      final sortedPos = activeVec < 0 ? 0 : svc.getSortedPosition(activeVec);
      final activeIdx = sortedPos < 0 ? 0 : sortedPos;
      final cur = _canvasNavNotifier.value;
      if (count != cur.count || activeIdx != cur.active) {
        _canvasNavNotifier.value = (count: count, active: activeIdx);
      }
      // Update canvas aspect ratio for texture sizing
      if (_isCanvasViewMode) {
        final sz = svc.getPanoramaCanvasSize();
        if (sz.width > 0 && sz.height > 0) {
          final newAr = sz.width / sz.height;
          if (_canvasAspectRatio == null ||
              (newAr - _canvasAspectRatio!).abs() > 0.01) {
            setState(() {
              _canvasAspectRatio = newAr;
            });
          }
        }
      }
      await _refreshGraphHistoryState();
      if (!mounted) return;
      // Poll multi-canvas lifecycle events and show toasts
      final event = svc.getLastLifecycleEvent();
      if (event != 0 && mounted) {
        final type = event & 0xFF;
        final targetIdx = (event >> 8) & 0xFFFF;
        String? msg;
        if (type == 1) msg = 'Resumed canvas ${targetIdx + 1}';
        if (type == 2) msg = 'Started new canvas ${targetIdx + 1}';
        if (type == 3) msg = 'Canvases merged';
        if (msg != null) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text(msg), duration: const Duration(seconds: 2)),
          );
        }
      }
      // Poll video file progress / completion
      if (_isVideoFileMode) {
        final progress = svc.getVideoProgress();
        final complete = svc.isVideoComplete();
        if (complete && !_isVideoComplete) {
          setState(() {
            _isVideoPaused = true;
            _videoProgress = 1.0;
            _isVideoComplete = true;
          });
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('Video processing complete')),
          );
        } else if (!_isSeekingVideo &&
            (progress - _videoProgress).abs() > 0.005) {
          setState(() {
            _videoProgress = progress;
          });
        }
      }
    });
  }

  void _stopCanvasPollTimer() {
    _canvasPollTimer?.cancel();
    _canvasPollTimer = null;
    _canvasNavNotifier.value = (count: 0, active: 0);
    _resetWhiteboardUiState();
  }

  void _setCanvasViewMode(bool enabled) {
    setState(() {
      _isCanvasViewMode = enabled;
      _canvasAspectRatio = enabled ? _canvasAspectRatio : null;
      _currentZoom = 1.0;
      if (enabled) {
        _graphHistoryFollowLatest = true;
      }
    });
    NativeCameraService().setCanvasViewMode(enabled);
  }

  void _resetWhiteboardUiState() {
    _canvasNavNotifier.value = (count: 0, active: 0);
    _graphHistoryTimeline = const [];
    _selectedGraphHistoryIndex = -1;
    _graphHistoryPeakIndex = -1;
    _graphHistoryFollowLatest = true;
    _isDraggingGraphHistory = false;
    _graphHistoryDragValue = 0.0;
    _isVideoPaused = false;
  }

  void _setVideoPaused(bool paused) {
    NativeCameraService().setVideoPaused(paused);
    if (!mounted) {
      _isVideoPaused = paused;
      return;
    }
    setState(() {
      _isVideoPaused = paused;
    });
  }

  Widget _buildVideoPlaybackControls() {
    return SizedBox(
      height: 40,
      child: Row(
        children: [
          IconButton(
            onPressed: _isVideoFileMode
                ? () => _setVideoPaused(!_isVideoPaused)
                : null,
            icon: Icon(_isVideoPaused ? Icons.play_arrow : Icons.pause),
            tooltip: _isVideoPaused ? 'Play video' : 'Pause video',
          ),
          Expanded(
            child: SliderTheme(
              data: SliderTheme.of(context).copyWith(
                trackHeight: 4,
                thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 7),
                overlayShape: const RoundSliderOverlayShape(
                  overlayRadius: 14,
                ),
                activeTrackColor: Colors.blue,
                inactiveTrackColor: Colors.lightBlue.withAlpha(90),
                thumbColor: Colors.blue,
              ),
              child: Slider(
                value: (_isSeekingVideo ? _seekDragValue : _videoProgress)
                    .clamp(0.0, 1.0),
                min: 0.0,
                max: 1.0,
                onChangeStart: (v) {
                  setState(() {
                    _isSeekingVideo = true;
                    _seekDragValue = v;
                  });
                },
                onChanged: (v) {
                  setState(() => _seekDragValue = v);
                },
                onChangeEnd: (v) {
                  NativeCameraService().seekVideoToProgress(v);
                  setState(() {
                    _isSeekingVideo = false;
                    _videoProgress = v;
                    _isVideoComplete = false;
                    _isVideoPaused = false;
                  });
                },
              ),
            ),
          ),
        ],
      ),
    );
  }

  bool _sameGraphHistoryTimeline(
    List<GraphHistoryTimelineEntry> left,
    List<GraphHistoryTimelineEntry> right,
  ) {
    if (identical(left, right)) return true;
    if (left.length != right.length) return false;
    for (int i = 0; i < left.length; i++) {
      if (left[i].frameId != right[i].frameId ||
          left[i].nodeCount != right[i].nodeCount) {
        return false;
      }
    }
    return true;
  }

  Future<void> _refreshGraphHistoryState() async {
    if (!mounted || !Platform.isWindows || !_isWhiteboardMode) return;

    final svc = NativeCameraService();
    var timeline = svc.getGraphHistoryTimeline();
    final latestIndex = timeline.isEmpty ? -1 : timeline.length - 1;
    if (_graphHistoryFollowLatest && timeline.isNotEmpty) {
      if (svc.getGraphHistorySelectedIndex() != latestIndex) {
        svc.setGraphHistorySelectedIndex(latestIndex);
      }
    }

    final rawSelectedIndex = svc.getGraphHistorySelectedIndex();
    final peakIndex = svc.getGraphHistoryPeakIndex();
    final selectedIndex = timeline.isEmpty
        ? -1
        : (_graphHistoryFollowLatest
              ? latestIndex
              : rawSelectedIndex.clamp(0, timeline.length - 1));

    if (!mounted) return;

    final shouldUpdate = !_sameGraphHistoryTimeline(
          timeline,
          _graphHistoryTimeline,
        ) ||
        selectedIndex != _selectedGraphHistoryIndex ||
        peakIndex != _graphHistoryPeakIndex;

    if (!shouldUpdate) return;

    setState(() {
      _graphHistoryTimeline = timeline;
      _selectedGraphHistoryIndex = selectedIndex;
      _graphHistoryPeakIndex = peakIndex;
      if (!_isDraggingGraphHistory && selectedIndex >= 0) {
        _graphHistoryDragValue = selectedIndex.toDouble();
      }
    });
  }

  Future<void> _selectGraphHistoryIndex(
    int index, {
    required bool followLatest,
  }) async {
    final svc = NativeCameraService();
    svc.setGraphHistorySelectedIndex(index);

    setState(() {
      _graphHistoryFollowLatest = followLatest;
      _selectedGraphHistoryIndex = index;
      _graphHistoryDragValue = index.toDouble();
    });

    for (int attempt = 0; attempt < 8; attempt++) {
      if (!mounted) return;
      final currentIndex = svc.getGraphHistorySelectedIndex();
      if (currentIndex == index) break;
      await Future.delayed(const Duration(milliseconds: 30));
    }

    await _refreshGraphHistoryState();
  }

  // Renders one canvas+historyIndex to a PNG image, using the direct per-canvas FFI.
  Future<({Uint8List bytes, int width, int height})?> _buildCanvasAtHistory(
      int canvasIdx, int historyIdx) async {
    final svc = NativeCameraService();
    final canvasSize = svc.getPanoramaCanvasSize();
    final maxW = canvasSize.width > 0
        ? canvasSize.width.ceil().clamp(1, 16384)
        : 1920;
    final maxH = canvasSize.height > 0
        ? canvasSize.height.ceil().clamp(1, 16384)
        : 1080;
    final result = svc.getSubCanvasOverviewRgba(
        canvasIdx, historyIdx, width: maxW, height: maxH);
    if (result == null) return null;
    final image = await ui.ImmutableBuffer.fromUint8List(result.bytes)
        .then((buf) => ui.ImageDescriptor.raw(buf,
            width: result.width,
            height: result.height,
            pixelFormat: ui.PixelFormat.rgba8888))
        .then((d) => d.instantiateCodec())
        .then((c) => c.getNextFrame())
        .then((f) => f.image);
    final byteData = await image.toByteData(format: ui.ImageByteFormat.png);
    if (byteData == null) return null;
    return (bytes: byteData.buffer.asUint8List(),
            width: result.width,
            height: result.height);
  }

  Future<void> _addPeakGraphToGallery() async {
    final svc = NativeCameraService();
    const kStaleThreshold = 5;
    final canvasCount = svc.getSubCanvasCount();
    if (canvasCount == 0) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('No canvas data available.')),
      );
      return;
    }

    int added = 0;
    for (int ci = 0; ci < canvasCount; ci++) {
      final peakIdx = svc.getSubCanvasPeakIndex(ci);
      final histCount = svc.getSubCanvasHistoryCount(ci);
      final latestIdx = histCount - 1;

      if (peakIdx >= 0) {
        final img = await _buildCanvasAtHistory(ci, peakIdx);
        if (img != null) {
          GalleryService.instance.addImage(img.bytes, img.width, img.height);
          added++;
        }
      }

      // Also add latest if it's sufficiently ahead of peak
      final peakIsStale =
          peakIdx < 0 || (latestIdx - peakIdx >= kStaleThreshold);
      if (peakIsStale && latestIdx >= 0) {
        final img = await _buildCanvasAtHistory(ci, latestIdx);
        if (img != null) {
          GalleryService.instance.addImage(img.bytes, img.width, img.height);
          added++;
        }
      }

      if (!mounted) return;
    }

    if (!mounted) return;
    if (added == 0) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('No peak graph images were available.')),
      );
      return;
    }

    setState(() {
      _isSidebarOpen = true;
      _isGalleryFullScreen = false;
      _currentGalleryIndex = GalleryService.instance.images.length - 1;
    });

    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Added $added peak graph image(s) to gallery.')),
    );
  }

  Future<void> _restoreGraphHistorySelection({
    required int previousSelectedIndex,
    required bool previousFollowLatest,
  }) async {
    if (!mounted) return;
    if (previousSelectedIndex >= 0) {
      await _selectGraphHistoryIndex(
        previousSelectedIndex,
        followLatest: previousFollowLatest,
      );
      return;
    }
    if (previousFollowLatest && _graphHistoryTimeline.isNotEmpty) {
      await _selectGraphHistoryIndex(
        _graphHistoryTimeline.length - 1,
        followLatest: true,
      );
    } else {
      setState(() {
        _graphHistoryFollowLatest = previousFollowLatest;
      });
    }
  }

  Future<void> _addPeakGraphToGalleryPreservingSelection() async {
    final previousFollowLatest = _graphHistoryFollowLatest;
    final previousSelectedIndex = _selectedGraphHistoryIndex;
    try {
      await _addPeakGraphToGallery();
    } finally {
      if (mounted) {
        await _restoreGraphHistorySelection(
          previousSelectedIndex: previousSelectedIndex,
          previousFollowLatest: previousFollowLatest,
        );
      }
    }
  }

  void _toggleWhiteboardSensitivity() {
    final newHighSensitivity = !_isHighWhiteboardSensitivity;
    NativeCameraService().setAbsenceScoreSeenThreshold(
      newHighSensitivity ? 0.0 : 1.0,
    );
    setState(() {
      _isHighWhiteboardSensitivity = newHighSensitivity;
    });
  }

  Future<void> _loadPdfPath() async {
    final initialPath = await DocumentService.instance.getInitialPdfPath();

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
    _isVddCaptureMode = widget.isVddCapture;

    // Set default max zoom based on platform
    // Windows cameras usually don't support optical zoom commands, so we limit to 1.0
    // to prevent "empty" zoom range where nothing happens visually.
    // If connecting to a remote stream later, this can be updated.
    _phoneMaxZoom = Platform.isWindows ? 1.0 : 10.0;

    // Enable digital zoom override by default for native cameras (Windows)
    if (Platform.isWindows) {
      _isDigitalZoomOverride = true;
      _isHighWhiteboardSensitivity =
          NativeCameraService().getAbsenceScoreSeenThreshold() <= 0.5;
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
    } else if (widget.initialVideoFilePath != null) {
      _isStreamMode = true;
      WidgetsBinding.instance.addPostFrameCallback((_) async {
        await _connectToStream(widget.initialVideoFilePath!);
        if (!mounted) return;
        final svc = NativeCameraService();
        svc.setPanoramaEnabled(true);
        svc.setCanvasViewMode(true);
        setState(() {
          _isVideoFileMode = true;
          _isVideoPaused = false;
          _videoProgress = 0.0;
          _isWhiteboardMode = true;
          _isCanvasViewMode = true;
        });
        _startCanvasPollTimer();
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
      overrideAspectRatio: _isCanvasViewMode ? _canvasAspectRatio : null,
      transformationController: _isCanvasViewMode
          ? _canvasTransformationController
          : null,
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

      // If screen capture is already active, don't start the local camera
      if (NativeCameraService().isScreenCaptureActive()) {
        return;
      }

      if (widget.initialCameraIndex != null) {
        _selectedCameraIndex = widget.initialCameraIndex!;
        // Yield to event loop to ensure scheduler is idle before calling native code
        // This prevents the "SchedulerPhase.idle" assertion error
        WidgetsBinding.instance.addPostFrameCallback((_) async {
          await Future.delayed(Duration.zero);
          // Re-check if screen capture is active, since this callback runs async
          // and screen capture might have been started after _init() began
          if (!NativeCameraService().isScreenCaptureActive()) {
            NativeCameraService().selectCamera(_selectedCameraIndex);
          }
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
            _switchToSource();

            setState(() {
              _isStreamMode = false;
              _isDigitalZoomOverride = true;
              _lockedPhoneZoom = 1.0;
              _currentZoom = 1.0;
            });

            NativeCameraService().selectCamera(index);
          },
          onSelectMobile: () {
            Navigator.pop(context);
            _switchToSource();

            showDialog(
              context: context,
              builder: (context) => MobileConnectionDialog(
                onConnect: () {
                  Navigator.of(context).pop();
                  _connectToStream('rtmp://localhost/live/stream');
                },
              ),
            );
          },
          onSelectScreenCapture: (monitorIndex, windowHandle) {
            _switchToSource();

            final success = NativeCameraService().startScreenCapture(
              monitorIndex,
              windowHandle: windowHandle,
            );
            if (success) {
              setState(() {
                _isStreamMode = false;
                _isDigitalZoomOverride = true;
                _lockedPhoneZoom = 1.0;
                _currentZoom = 1.0;
              });
            } else {
              ScaffoldMessenger.of(context).showSnackBar(
                SnackBar(
                  content: Text(
                    AppLocalizations.of(context)!.failedToStartScreenCapture,
                  ),
                ),
              );
            }
          },
          onSelectVideoFile: (path) async {
            Navigator.pop(context);
            _switchToSource();
            await _connectToStream(path);
            if (!mounted) return;
            final svc = NativeCameraService();
            svc.setPanoramaEnabled(true);
            svc.setCanvasViewMode(true);
            setState(() {
              _isDigitalZoomOverride = true;
              _lockedPhoneZoom = 1.0;
              _currentZoom = 1.0;
              _isWhiteboardMode = true;
              _isCanvasViewMode = true;
              _isVideoFileMode = true;
              _isVideoPaused = false;
              _videoProgress = 0.0;
            });
            _startCanvasPollTimer();
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

  /// Forward a screen tap to the virtual display, accounting for zoom and pan.
  /// [tapPos] is the local position within the video widget.
  /// [viewportSize] is the size of the video widget.
  /// [clickType] 0=left, 1=right.
  void _forwardClickToVirtualDisplay(
    Offset tapPos,
    Size viewportSize,
    int clickType,
  ) {
    final Offset scenePoint = _transformationController.toScene(tapPos);

    // Normalize to 0-1 range relative to viewport
    double normalizedX = scenePoint.dx / viewportSize.width;
    double normalizedY = scenePoint.dy / viewportSize.height;

    // If live crop is active, map display coords back to original frame coords
    if (_isLiveCropActive) {
      final mapped = NativeCameraService().mapDisplayToOriginal(
        normalizedX.clamp(0.0, 1.0),
        normalizedY.clamp(0.0, 1.0),
      );
      normalizedX = mapped.x;
      normalizedY = mapped.y;
    }

    NativeCameraService().sendClickToVirtualDisplay(
      normalizedX.clamp(0.0, 1.0),
      normalizedY.clamp(0.0, 1.0),
      clickType: clickType,
    );
  }

  /// Forward a scroll event to the virtual display, accounting for zoom and pan.
  void _forwardScrollToVirtualDisplay(
    Offset scrollPos,
    Size viewportSize,
    double scrollDeltaY,
  ) {
    final Offset scenePoint = _transformationController.toScene(scrollPos);

    double normalizedX = scenePoint.dx / viewportSize.width;
    double normalizedY = scenePoint.dy / viewportSize.height;

    // If live crop is active, map display coords back to original frame coords
    if (_isLiveCropActive) {
      final mapped = NativeCameraService().mapDisplayToOriginal(
        normalizedX.clamp(0.0, 1.0),
        normalizedY.clamp(0.0, 1.0),
      );
      normalizedX = mapped.x;
      normalizedY = mapped.y;
    }

    // Convert Flutter scroll delta to Windows WHEEL_DELTA units
    // Flutter gives pixels, Windows expects multiples of 120 (WHEEL_DELTA)
    // Negative delta in Flutter = scroll down, positive in Windows = scroll up
    final int wheelDelta = -(scrollDeltaY ~/ 2).clamp(-600, 600);

    NativeCameraService().sendScrollToVirtualDisplay(
      normalizedX.clamp(0.0, 1.0),
      normalizedY.clamp(0.0, 1.0),
      wheelDelta,
    );
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

  /// Stops all video sources and clears any active crop.
  void _switchToSource() {
    // Stop ALL sources before starting new one
    NativeCameraService().stop(); // Stop camera/stream
    NativeCameraService().stopScreenCapture(); // Stop screen capture
    NativeCameraService().setVideoPaused(false);

    // Clear crop when switching inputs
    NativeCameraService().setLiveCropCorners(null);
    setState(() {
      _isLiveCropActive = false;
      _isVideoPaused = false;
    });
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    WakelockPlus.disable();
    _controller?.dispose();
    unawaited(_stopMobileRtmpStreaming(fromDispose: true));
    _transformationController.dispose();
    _canvasTransformationController.dispose();
    _clearActiveStreamTracking();

    _mediaServerSubscription?.cancel();
    _streamStoppedSubscription?.cancel();

    if (Platform.isWindows) {
      NativeCameraService().stop();
    }

    FiltersService.instance.removeListener(_onFiltersChanged);
    GalleryService.instance.removeListener(_onGalleryChange);

    _canvasPollTimer?.cancel();
    _canvasNavNotifier.dispose();
    _flashController.dispose();
    super.dispose();
  }

  Future<void> _startLocalCamera() async {
    if (Platform.isWindows) {
      if (!NativeCameraService().isScreenCaptureActive()) {
        NativeCameraService().start();
      }
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

  /// Captures the full canvas image (not just the visible portion).
  Future<void> _captureFullCanvas() async {
    if (!Platform.isWindows) return;

    _flashController.forward(from: 0.0);

    try {
      // Use actual canvas size so capture is not downscaled
      final canvasSize = NativeCameraService().getPanoramaCanvasSize();
      final maxDim = canvasSize.width > 0 && canvasSize.height > 0
          ? canvasSize.width.ceil().clamp(1, 16384)
          : 4096;
      final maxDimH = canvasSize.height > 0
          ? canvasSize.height.ceil().clamp(1, 16384)
          : 4096;
      final result = NativeCameraService().getCanvasFullResRgba(
        maxWidth: maxDim,
        maxHeight: maxDimH,
      );
      if (result == null) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('Failed to get canvas image')),
          );
        }
        return;
      }

      // Convert RGBA to PNG
      final image = await ui.ImmutableBuffer.fromUint8List(result.bytes)
          .then(
            (buffer) => ui.ImageDescriptor.raw(
              buffer,
              width: result.width,
              height: result.height,
              pixelFormat: ui.PixelFormat.rgba8888,
            ),
          )
          .then((descriptor) => descriptor.instantiateCodec())
          .then((codec) => codec.getNextFrame())
          .then((frame) => frame.image);

      final byteData = await image.toByteData(format: ui.ImageByteFormat.png);
      if (!mounted || byteData == null) return;

      final pngBytes = byteData.buffer.asUint8List();
      GalleryService.instance.addImage(pngBytes, result.width, result.height);
    } catch (e) {
      debugPrint('Error capturing full canvas: $e');
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Error capturing canvas: $e')));
    }
  }

  /// Captures the current frame and opens CropScreen for 4-point perspective crop.
  Future<void> _captureAndCrop() async {
    if (!Platform.isWindows) return;

    try {
      // Capture the current frame using RepaintBoundary
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

      // Navigate to CropScreen with returnCornersOnly to get corner coordinates
      final result = await Navigator.push<List<Offset>>(
        context,
        MaterialPageRoute(
          builder: (_) =>
              CropScreen(imageBytes: pngBytes, returnCornersOnly: true),
        ),
      );

      if (!mounted) return;

      // If user confirmed the crop, apply it to live feed
      if (result != null && result.length == 4) {
        NativeCameraService().setLiveCropCorners(result);
        setState(() {
          _isLiveCropActive = true;
        });
      }
    } catch (e) {
      debugPrint('Error in _captureAndCrop: $e');
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
      await DocumentService.instance.saveLastPdfPath(selectedDirectory);

      setState(() {
        _pdfPathController.text = selectedDirectory;
      });
    }
  }

  Future<void> _exportPdf() async {
    if (_capturedImages.isEmpty) return;

    try {
      final file = await DocumentService.instance.exportPdf(
        images: _capturedImages,
        fileName: _pdfNameController.text.trim(),
        directoryPath: _pdfPathController.text.trim(),
      );

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

  void _showLiveShareDialog() {
    if (LiveShareService().isBroadcasting) {
      LiveShareService().stopBroadcasting();
      setState(() {});
      return;
    }

    final String boardId = (100000 + Random().nextInt(900000)).toString(); 
    final String host = 'kaptchi.com'; 
    final String wsUrl = 'wss://$host/api/ws?id=$boardId&role=host';
    final String webUrl = 'https://$host/share?id=$boardId';

    showDialog(
      context: context,
      builder: (context) {
        return AlertDialog(
          title: const Text('Live Share Canvas'),
          content: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text('Share your live canvas to the web viewer:'),
              const SizedBox(height: 16),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                decoration: BoxDecoration(
                  color: Colors.grey.withAlpha(50),
                  borderRadius: BorderRadius.circular(8),
                  border: Border.all(color: Colors.grey.withAlpha(50)),
                ),
                child: SelectableText(webUrl, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
              ),
              const SizedBox(height: 16),
              const Text('Anyone with this link can view the canvas live.'),
            ],
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('Cancel'),
            ),
            ElevatedButton(
              onPressed: () {
                LiveShareService().startBroadcasting(wsUrl, boardId);
                Navigator.pop(context);
                setState(() {});
              },
              child: const Text('Start Sharing'),
            ),
          ],
        );
      },
    );
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
          bottom: _isVideoFileMode
              ? (_isCanvasViewMode && _graphHistoryTimeline.isNotEmpty
                    ? PreferredSize(
                        preferredSize: const Size.fromHeight(124),
                        child: Padding(
                          padding: const EdgeInsets.fromLTRB(12, 4, 12, 10),
                          child: Column(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Row(
                                children: [
                                  Text(
                                    'Graph ${(_selectedGraphHistoryIndex >= 0 ? _selectedGraphHistoryIndex + 1 : 0)}/${_graphHistoryTimeline.length}',
                                    style: const TextStyle(fontSize: 12),
                                  ),
                                  const Spacer(),
                                  if (_selectedGraphHistoryIndex >= 0 &&
                                      _selectedGraphHistoryIndex <
                                          _graphHistoryTimeline.length)
                                    Text(
                                      '${_graphHistoryTimeline[_selectedGraphHistoryIndex].nodeCount} nodes',
                                      style: const TextStyle(fontSize: 12),
                                    ),
                                  if (_selectedGraphHistoryIndex ==
                                      _graphHistoryPeakIndex)
                                    const Padding(
                                      padding: EdgeInsets.only(left: 8),
                                      child: Text(
                                        'peak',
                                        style: TextStyle(
                                          fontSize: 12,
                                          fontWeight: FontWeight.w700,
                                          color: Colors.orange,
                                        ),
                                      ),
                                    ),
                                ],
                              ),
                              const SizedBox(height: 6),
                              GraphHistorySparkline(
                                timeline: _graphHistoryTimeline,
                                selectedIndex: _selectedGraphHistoryIndex,
                                peakIndex: _graphHistoryPeakIndex,
                              ),
                              const SizedBox(height: 4),
                              SliderTheme(
                                data: SliderTheme.of(context).copyWith(
                                  trackHeight: 4,
                                  thumbShape: const RoundSliderThumbShape(
                                    enabledThumbRadius: 7,
                                  ),
                                  overlayShape: const RoundSliderOverlayShape(
                                    overlayRadius: 14,
                                  ),
                                  activeTrackColor: Colors.teal,
                                  inactiveTrackColor: Colors.grey.withAlpha(80),
                                  thumbColor: Colors.teal,
                                ),
                                child: Slider(
                                  value: (_isDraggingGraphHistory
                                          ? _graphHistoryDragValue
                                          : _selectedGraphHistoryIndex.toDouble())
                                      .clamp(
                                        0.0,
                                        (_graphHistoryTimeline.length - 1)
                                            .toDouble(),
                                      ),
                                  min: 0.0,
                                  max: (_graphHistoryTimeline.length - 1)
                                      .toDouble(),
                                  onChangeStart: (value) {
                                    setState(() {
                                      _isDraggingGraphHistory = true;
                                      _graphHistoryDragValue = value;
                                    });
                                  },
                                  onChanged: (value) {
                                    setState(() => _graphHistoryDragValue = value);
                                  },
                                  onChangeEnd: (value) {
                                    final index = value.round();
                                    final latestIndex =
                                        _graphHistoryTimeline.length - 1;
                                    setState(() {
                                      _isDraggingGraphHistory = false;
                                      _graphHistoryDragValue = value;
                                    });
                                    unawaited(
                                      _selectGraphHistoryIndex(
                                        index,
                                        followLatest: index >= latestIndex,
                                      ),
                                    );
                                  },
                                ),
                              ),
                              const SizedBox(height: 4),
                              _buildVideoPlaybackControls(),
                            ],
                          ),
                        ),
                      )
                    : PreferredSize(
                        preferredSize: const Size.fromHeight(44),
                        child: Padding(
                          padding: const EdgeInsets.symmetric(horizontal: 8),
                          child: _buildVideoPlaybackControls(),
                        ),
                      ))
              : null,
          title: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(AppLocalizations.of(context)!.cameraMode),
              if (Platform.isWindows && _isWhiteboardMode)
                ValueListenableBuilder<({int count, int active})>(
                  valueListenable: _canvasNavNotifier,
                  builder: (ctx, nav, _) {
                    if (nav.count == 0) return const SizedBox.shrink();
                    return Padding(
                      padding: const EdgeInsets.only(left: 8),
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 8,
                          vertical: 2,
                        ),
                        decoration: BoxDecoration(
                          color: Colors.blue.withAlpha(200),
                          borderRadius: BorderRadius.circular(10),
                        ),
                        child: Text(
                          '${nav.active + 1}/${nav.count}',
                          style: const TextStyle(
                            fontSize: 12,
                            color: Colors.white,
                          ),
                        ),
                      ),
                    );
                  },
                ),
            ],
          ),
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
            // Crop Button - set or clear live perspective crop
            IconButton(
              icon: Icon(
                _isLiveCropActive ? Icons.crop_free : Icons.crop,
                color: _isLiveCropActive ? Colors.green : null,
              ),
              tooltip: _isLiveCropActive ? 'Clear Crop' : 'Crop Frame',
              onPressed: () {
                if (_isLiveCropActive) {
                  // Clear the crop
                  NativeCameraService().setLiveCropCorners(null);
                  setState(() {
                    _isLiveCropActive = false;
                  });
                } else {
                  // Set new crop
                  _captureAndCrop();
                }
              },
            ),
            // VDD Scroll Mode Toggle (zoom vs forward scroll)
            if (_isVddCaptureMode)
              IconButton(
                icon: Icon(
                  _vddScrollForwardsToDisplay ? Icons.mouse : Icons.zoom_in,
                  color: _vddScrollForwardsToDisplay ? Colors.deepPurple : null,
                ),
                tooltip: _vddScrollForwardsToDisplay
                    ? 'Scroll: forwarding to display (click to switch to zoom)'
                    : 'Scroll: zooming (click to switch to forward)',
                onPressed: () {
                  setState(() {
                    _vddScrollForwardsToDisplay = !_vddScrollForwardsToDisplay;
                  });
                },
              ),
            // Whiteboard Capture Toggle
            if (Platform.isWindows)
              IconButton(
                icon: Icon(
                  _isWhiteboardMode
                      ? Icons.document_scanner
                      : Icons.document_scanner_outlined,
                  color: _isWhiteboardMode ? Colors.blue : null,
                ),
                tooltip: _isWhiteboardMode
                    ? AppLocalizations.of(context)!.disableWhiteboard
                    : AppLocalizations.of(context)!.enableWhiteboard,
                onPressed: () {
                  final enableWhiteboard = !_isWhiteboardMode;

                  setState(() {
                    _isWhiteboardMode = enableWhiteboard;
                  });

                  if (enableWhiteboard) {
                    _startCanvasPollTimer();
                  } else {
                    _stopCanvasPollTimer();
                    if (_isCanvasViewMode) {
                      _setCanvasViewMode(false);
                    }
                  }

                  NativeCameraService().setPanoramaEnabled(enableWhiteboard);
                },
              ),
            if (Platform.isWindows && _isWhiteboardMode)
              IconButton(
                icon: Icon(
                  _isCanvasViewMode ? Icons.live_tv : Icons.image_outlined,
                  color: _isCanvasViewMode ? Colors.green : null,
                ),
                tooltip: AppLocalizations.of(context)!.toggleCanvasView,
                onPressed: () {
                  _setCanvasViewMode(!_isCanvasViewMode);
                },
              ),
            if (Platform.isWindows && _isWhiteboardMode)
              IconButton(
                icon: Icon(
                  LiveShareService().isBroadcasting
                      ? Icons.cast_connected
                      : Icons.cast,
                  color: LiveShareService().isBroadcasting ? Colors.orange : Colors.greenAccent,
                ),
                tooltip: LiveShareService().isBroadcasting
                    ? 'Stop Sharing Canvas'
                    : 'Share Canvas Live',
                onPressed: _showLiveShareDialog,
              ),
            if (Platform.isWindows && _isWhiteboardMode)
              IconButton(
                icon: Icon(
                  _isHighWhiteboardSensitivity
                      ? Icons.sensors
                      : Icons.sensors_outlined,
                  color: _isHighWhiteboardSensitivity ? Colors.orange : null,
                ),
                tooltip: _isHighWhiteboardSensitivity
                    ? 'Sensitivity: High'
                    : 'Sensitivity: Normal',
                onPressed: _toggleWhiteboardSensitivity,
              ),
            if (Platform.isWindows && _isWhiteboardMode)
              IconButton(
                icon: const Icon(Icons.auto_graph),
                color: _graphHistoryTimeline.isNotEmpty ? Colors.tealAccent : null,
                tooltip: 'Add Peak Graph To Gallery',
                onPressed: _graphHistoryTimeline.isNotEmpty
                    ? () {
                        unawaited(_addPeakGraphToGalleryPreservingSelection());
                      }
                    : null,
              ),
            // Edit Canvas (only visible when whiteboard mode is active)
            if (Platform.isWindows && _isWhiteboardMode)
              IconButton(
                icon: const Icon(Icons.edit_note),
                tooltip: 'Edit Canvas',
                onPressed: () {
                  Navigator.push(
                    context,
                    MaterialPageRoute(builder: (_) => const EditCanvasScreen()),
                  );
                },
              ),
            // Capture full canvas (only visible in canvas view mode)
            if (Platform.isWindows && _isCanvasViewMode)
              IconButton(
                icon: const Icon(Icons.photo_camera, color: Colors.amber),
                tooltip: 'Capture full canvas',
                onPressed: _captureFullCanvas,
              ),
            // Reset Whiteboard (only visible when whiteboard mode is active)
            if (Platform.isWindows && _isWhiteboardMode)
              IconButton(
                icon: const Icon(Icons.refresh),
                tooltip: AppLocalizations.of(context)!.resetWhiteboard,
                onPressed: () {
                  NativeCameraService().resetPanorama();
                  setState(_resetWhiteboardUiState);
                },
              ),
            // Merge canvases (only when 2+ sub-canvases exist)
            if (Platform.isWindows && _isWhiteboardMode)
              ValueListenableBuilder<({int count, int active})>(
                valueListenable: _canvasNavNotifier,
                builder: (ctx, nav, _) {
                  if (nav.count < 2) return const SizedBox.shrink();
                  return IconButton(
                    icon: const Icon(Icons.merge_type),
                    tooltip: 'Merge canvases',
                    onPressed: () {
                      final merged = NativeCameraService().tryMergeGroupsNow();
                      if (mounted) {
                        ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                          content: Text(merged
                              ? 'Canvases merged'
                              : 'Nothing to merge yet'),
                          duration: const Duration(seconds: 2),
                        ));
                      }
                    },
                  );
                },
              ),
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
                            child: Builder(
                              builder: (context) {
                                final windowsChild = _isStreamMode
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
                                                (
                                                  context,
                                                  processedImage,
                                                  child,
                                                ) {
                                                  bool hasActiveFilters =
                                                      FiltersService
                                                          .instance
                                                          .filters
                                                          .any(
                                                            (f) => f.isActive,
                                                          );
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
                                        : NativeTextureView(
                                        transformationController:
                                            _isCanvasViewMode
                                            ? _canvasTransformationController
                                            : null,
                                        overrideAspectRatio: _isCanvasViewMode
                                            ? _canvasAspectRatio
                                            : null,
                                      );

                                final videoStack = _isCanvasViewMode
                                    ? windowsChild
                                    : InteractiveViewer(
                                        transformationController:
                                            _transformationController,
                                        minScale: 1.0,
                                        maxScale: 50.0,
                                        child: windowsChild,
                                      );

                                // Wrap with tap forwarding when in VDD capture mode
                                if (!_isVddCaptureMode) return videoStack;
                                return LayoutBuilder(
                                  builder: (context, constraints) {
                                    return Listener(
                                      onPointerSignal:
                                          _vddScrollForwardsToDisplay
                                          ? (event) {
                                              if (event is PointerScrollEvent) {
                                                _forwardScrollToVirtualDisplay(
                                                  event.localPosition,
                                                  constraints.biggest,
                                                  event.scrollDelta.dy,
                                                );
                                              }
                                            }
                                          : null,
                                      child: GestureDetector(
                                        behavior: HitTestBehavior.translucent,
                                        onTapUp: (details) {
                                          _forwardClickToVirtualDisplay(
                                            details.localPosition,
                                            constraints.biggest,
                                            0,
                                          );
                                        },
                                        onSecondaryTapUp: (details) {
                                          _forwardClickToVirtualDisplay(
                                            details.localPosition,
                                            constraints.biggest,
                                            1,
                                          );
                                        },
                                        child: videoStack,
                                      ),
                                    );
                                  },
                                );
                              },
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

            // Sub-canvas navigation arrows (must be above edge detectors in z-order)
            if (Platform.isWindows && _isCanvasViewMode)
              ValueListenableBuilder<({int count, int active})>(
                valueListenable: _canvasNavNotifier,
                builder: (ctx, nav, _) {
                  if (nav.count <= 1) {
                    return const SizedBox.shrink();
                  }
                  return Positioned.fill(
                    child: Padding(
                      padding: const EdgeInsets.only(bottom: 110),
                      child: Row(
                        mainAxisAlignment: MainAxisAlignment.spaceBetween,
                        crossAxisAlignment: CrossAxisAlignment.center,
                        children: [
                          _SubCanvasArrowButton(
                            icon: Icons.chevron_left,
                            enabled: nav.active > 0,
                            onPressed: () {
                              final nextSorted = nav.active - 1;
                              final vecIdx = NativeCameraService()
                                  .getSortedSubCanvasIndex(nextSorted);
                              if (vecIdx < 0) return;
                              NativeCameraService().setActiveSubCanvas(vecIdx);
                              _canvasNavNotifier.value = (
                                count: nav.count,
                                active: nextSorted,
                              );
                            },
                          ),
                          const Spacer(),
                          _SubCanvasArrowButton(
                            icon: Icons.chevron_right,
                            enabled: nav.active < nav.count - 1,
                            onPressed: () {
                              final nextSorted = nav.active + 1;
                              final vecIdx = NativeCameraService()
                                  .getSortedSubCanvasIndex(nextSorted);
                              if (vecIdx < 0) return;
                              NativeCameraService().setActiveSubCanvas(vecIdx);
                              _canvasNavNotifier.value = (
                                count: nav.count,
                                active: nextSorted,
                              );
                            },
                          ),
                        ],
                      ),
                    ),
                  );
                },
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
              onParameterChanged: (filterId, key, value) {
                FiltersService.instance.updateParameter(filterId, key, value);
                NativeCameraService().setFilterParameter(filterId, value);
              },
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

/// Semi-transparent arrow button used for sub-canvas navigation.
class _SubCanvasArrowButton extends StatelessWidget {
  final IconData icon;
  final bool enabled;
  final VoidCallback onPressed;

  const _SubCanvasArrowButton({
    required this.icon,
    required this.enabled,
    required this.onPressed,
  });

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: enabled ? onPressed : null,
      child: Container(
        width: 48,
        height: 80,
        decoration: BoxDecoration(
          color: enabled ? Colors.black54 : Colors.black26,
          borderRadius: BorderRadius.circular(8),
        ),
        child: Icon(
          icon,
          color: enabled ? Colors.white : Colors.white38,
          size: 36,
        ),
      ),
    );
  }
}
