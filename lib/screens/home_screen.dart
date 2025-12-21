import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:kaptchi_flutter/services/raw_socket_service.dart';
import 'package:camera/camera.dart';
import 'package:mobile_scanner/mobile_scanner.dart';

import '../services/media_server_service.dart';
import 'package:window_manager/window_manager.dart';
import '../services/gallery_service.dart';
import 'camera_screen.dart';
import '../services/document_service.dart';
import '../widgets/home_screen_widgets.dart';

import 'package:kaptchi_flutter/l10n/app_localizations.dart';
import 'settings_screen.dart';
import 'info_screen.dart';

import 'package:file_picker/file_picker.dart';

import '../services/native_camera_service.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> with WindowListener {
  bool _isScanning = true;

  // Windows State
  List<CameraDescription> _cameras = [];
  String _serverIp = '';
  bool _isMediaServerRunning = false;
  String? _mediaServerPath;
  List<({String name, String ip})> _availableInterfaces = [];

  // Protocol Selection
  StreamSubscription? _streamSubscription;

  // PDF Export State
  final TextEditingController _pdfNameController = TextEditingController();
  final TextEditingController _pdfPathController = TextEditingController();

  @override
  void initState() {
    super.initState();
    if (Platform.isWindows &&
        !Platform.environment.containsKey('FLUTTER_TEST')) {
      _loadCameras();
      _startServer();
      // Don't auto-start media server, let user choose or try default if they want
      _startMediaServer();

      // Listen for RTMP stream start
      _streamSubscription = MediaServerService.instance.onStreamStarted.listen((
        path,
      ) {
        if (!mounted) return;
        // Always navigate if a stream starts, assuming user wants to see it
        // Construct the full URL
        final url = 'rtmp://localhost/$path';

        _navigateToCamera(url);
      });

      windowManager.addListener(this);
      _setWindowCloseHandler();
    }
    GalleryService.instance.addListener(_onGalleryChange);
    _loadInitialPdfPath();
  }

  Future<void> _loadInitialPdfPath() async {
    final initialPath = await DocumentService.instance.getInitialPdfPath();

    // Set initial config in service

    // Sync local controllers
    if (mounted) {
      setState(() {
        _pdfPathController.text = initialPath;
      });
    }
  }

  void _onGalleryChange() {
    if (mounted) {
      setState(() {});
    }
  }

  void _setWindowCloseHandler() async {
    await windowManager.setPreventClose(
      true,
    ); // Must set this to intercept close
  }

  @override
  void onWindowClose() async {
    bool shouldClose = true;
    if (GalleryService.instance.images.isNotEmpty) {
      final shouldExit = await showDialog<bool>(
        context: context,
        builder: (context) => AlertDialog(
          title: Text(AppLocalizations.of(context)!.unsavedImagesTitle),
          content: Text(AppLocalizations.of(context)!.unsavedImagesMessage),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: Text(AppLocalizations.of(context)!.cancel),
            ),
            TextButton(
              onPressed: () => Navigator.of(context).pop(true),
              child: Text(
                AppLocalizations.of(context)!.exit,
                style: const TextStyle(color: Colors.red),
              ),
            ),
          ],
        ),
      );
      shouldClose = shouldExit == true;
    }

    if (shouldClose) {
      // PROACTIVELY KILL BACKGROUND PROCESSES
      // This ensures we don't wait for timeouts or slow disconnects
      try {
        MediaServerService.instance.stopServer();
        RawSocketService.instance.stop();
      } catch (e) {
        debugPrint('Error during shutdown cleanup: $e');
      }

      await windowManager.destroy();
    }
  }

  @override
  void dispose() {
    if (Platform.isWindows) {
      windowManager.removeListener(this);
    }
    _streamSubscription?.cancel();
    MediaServerService.instance.stopServer();
    GalleryService.instance.removeListener(_onGalleryChange);
    super.dispose();
  }

  void _navigateToCamera(String url) {
    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => CameraScreen(initialStreamUrl: url)),
    ).then((_) {
      if (mounted) {
        _startServer();
      }
    });
  }

  Future<void> _startMediaServer() async {
    final success = await MediaServerService.instance.startServer(
      executablePath: _mediaServerPath,
    );
    if (mounted) {
      setState(() {
        _isMediaServerRunning = success;
      });
    }
  }

  Future<void> _loadCameras() async {
    try {
      _cameras = await availableCameras();
      if (mounted) setState(() {});
    } catch (e) {
      debugPrint('Error loading cameras: $e');
    }
  }

  Future<void> _startServer() async {
    try {
      final interfaces = await RawSocketService.instance.getNetworkInterfaces();

      if (mounted) {
        setState(() {
          _availableInterfaces = interfaces;
          if (interfaces.isNotEmpty) {
            _serverIp = interfaces[0].ip;
          }
        });
      }
    } catch (e) {
      debugPrint('Error getting network interfaces: $e');
    }
  }

  Future<void> _pickSaveDirectory() async {
    String? selectedDirectory = await FilePicker.platform.getDirectoryPath();
    if (selectedDirectory != null) {
      // Save this path as the last used one
      await DocumentService.instance.saveLastPdfPath(selectedDirectory);

      setState(() {
        _pdfPathController.text = selectedDirectory;
      });
    }
  }

  Future<void> _exportPdf() async {
    final images = GalleryService.instance.images;
    if (images.isEmpty) return;
    try {
      final file = await DocumentService.instance.exportPdf(
        images: images,
        fileName: _pdfNameController.text.trim(),
        directoryPath: _pdfPathController.text.trim(),
      );

      // Clear gallery after successful export
      GalleryService.instance.clear();

      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(AppLocalizations.of(context)!.pdfSaved(file.path)),
        ),
      );
    } catch (e) {
      debugPrint('Error exporting PDF: $e');
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(
            AppLocalizations.of(context)!.pdfExportError(e.toString()),
          ),
        ),
      );
    }
  }

  void _startScreenCapture(int monitorIndex, {int windowHandle = 0}) {
    final success = NativeCameraService().startScreenCapture(
      monitorIndex,
      windowHandle: windowHandle,
    );
    if (!success) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text(
              AppLocalizations.of(context)!.failedToStartScreenCapture,
            ),
          ),
        );
      }
      return;
    }

    Navigator.push(
      context,
      MaterialPageRoute(builder: (_) => const CameraScreen()),
    ).then((_) {
      NativeCameraService().stopScreenCapture();
      if (mounted) {
        _startServer();
      }
    });
  }

  void _showWindowPicker() {
    final windows = NativeCameraService().getCapturableWindows();

    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(AppLocalizations.of(context)!.selectWindowToCapture),
        content: SizedBox(
          width: 400,
          height: 400,
          child: ListView.builder(
            itemCount: windows.length,
            itemBuilder: (context, index) {
              final window = windows[index];
              return ListTile(
                leading: const Icon(Icons.window),
                title: Text(window.title, overflow: TextOverflow.ellipsis),
                onTap: () {
                  Navigator.of(context).pop();
                  // Window capture auto-detects monitor, so pass 0 as monitorIndex
                  _startScreenCapture(0, windowHandle: window.handle);
                },
              );
            },
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: Text(AppLocalizations.of(context)!.cancel),
          ),
        ],
      ),
    );
  }

  void _onDetect(BarcodeCapture capture) {
    if (!_isScanning) return;

    final List<Barcode> barcodes = capture.barcodes;
    for (final barcode in barcodes) {
      if (barcode.rawValue != null) {
        setState(() {
          _isScanning = false;
        });

        // Delay navigation to allow MobileScanner to dispose and release camera
        Future.delayed(const Duration(milliseconds: 500), () {
          if (!mounted) return;
          Navigator.push(
            context,
            MaterialPageRoute(
              builder: (_) => CameraScreen(connectionUrl: barcode.rawValue),
            ),
          ).then((_) {
            if (mounted) {
              setState(() {
                _isScanning = true;
              });
            }
          });
        });
        return;
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    if (Platform.isAndroid || Platform.isIOS) {
      return Scaffold(
        appBar: AppBar(title: Text(AppLocalizations.of(context)!.scanQrCode)),
        body: _isScanning
            ? MobileScanner(onDetect: _onDetect)
            : const Center(child: CircularProgressIndicator()),
      );
    }

    return Scaffold(
      appBar: AppBar(
        title: Text(AppLocalizations.of(context)!.appTitle),
        actions: [
          IconButton(
            icon: const Icon(Icons.info_outline),
            tooltip: AppLocalizations.of(context)!.aboutTooltip,
            onPressed: () {
              Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const InfoScreen()),
              );
            },
          ),
          IconButton(
            icon: const Icon(Icons.settings),
            tooltip: AppLocalizations.of(context)!.settings,
            onPressed: () {
              Navigator.push(
                context,
                MaterialPageRoute(builder: (_) => const SettingsScreen()),
              );
            },
          ),
        ],
      ),
      body: Row(
        children: [
          // Left side: Camera List
          Expanded(
            flex: 1,
            child: Container(
              color: Colors.black,
              child: SingleChildScrollView(
                child: Column(
                  children: [
                    Padding(
                      padding: EdgeInsets.all(16.0),
                      child: Text(
                        AppLocalizations.of(context)!.availableCameras,
                        style: const TextStyle(
                          fontSize: 20,
                          fontWeight: FontWeight.bold,
                          color: Colors.white,
                        ),
                      ),
                    ),
                    CameraListWidget(
                      cameras: _cameras,
                      shrinkWrap: true,
                      physics: const NeverScrollableScrollPhysics(),
                      onSelectCamera: (index) {
                        // Fix for swapped cameras when exactly 2 are present
                        int targetIndex = index;
                        if (_cameras.length == 2) {
                          targetIndex = (index == 0) ? 1 : 0;
                        }

                        Navigator.push(
                          context,
                          MaterialPageRoute(
                            builder: (_) =>
                                CameraScreen(initialCameraIndex: targetIndex),
                          ),
                        ).then((_) {
                          if (mounted) {
                            _startServer();
                          }
                        });
                      },
                    ),

                    ScreenCaptureWidget(
                      onSelectScreen: _startScreenCapture,
                      onSelectWindow: _showWindowPicker,
                    ),
                  ],
                ),
              ),
            ),
          ),

          // Right side: QR Code for Mobile Connection
          Expanded(
            flex: 1,
            child: Container(
              color: Colors.black,
              child: SingleChildScrollView(
                child: Column(
                  children: [
                    MobileConnectionSection(
                      availableInterfaces: _availableInterfaces,
                      serverIp: _serverIp,
                      isMediaServerRunning: _isMediaServerRunning,
                      onSelectIp: (val) {
                        if (val != null) {
                          setState(() {
                            _serverIp = val;
                          });
                        }
                      },
                    ),
                    ExportSectionWidget(
                      pdfNameController: _pdfNameController,
                      pdfPathController: _pdfPathController,
                      onPickDirectory: _pickSaveDirectory,
                      onExportPdf: _exportPdf,
                    ),
                    const SizedBox(height: 50),
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
