import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:kaptchi_flutter/services/raw_socket_service.dart';
import 'package:camera/camera.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:qr_flutter/qr_flutter.dart';
import '../services/media_server_service.dart';
import 'package:window_manager/window_manager.dart';
import '../services/gallery_service.dart';
import 'camera_screen.dart';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';

import 'package:file_picker/file_picker.dart';
import 'package:shared_preferences/shared_preferences.dart';

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
    final prefs = await SharedPreferences.getInstance();
    final String? lastPath = prefs.getString('last_pdf_path');

    String initialPath;
    if (lastPath != null) {
      initialPath = lastPath;
    } else {
      final dir = await getApplicationDocumentsDirectory();
      initialPath = dir.path;
    }

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
          title: const Text('Unsaved Images'),
          content: const Text(
            'You have images in your gallery that are not exported. They will be lost if you exit. Are you sure?',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: const Text('Cancel'),
            ),
            TextButton(
              onPressed: () => Navigator.of(context).pop(true),
              child: const Text('Exit', style: TextStyle(color: Colors.red)),
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
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('last_pdf_path', selectedDirectory);

      setState(() {
        _pdfPathController.text = selectedDirectory;
      });
    }
  }

  Future<void> _exportPdf() async {
    final images = GalleryService.instance.images;
    if (images.isEmpty) return;

    try {
      final pdf = pw.Document();

      for (final item in images) {
        final image = pw.MemoryImage(item.bytes);
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

      // Persist the LAST USED path (whether picked or typed/default)
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('last_pdf_path', dirPath);

      // Clear gallery after successful export
      GalleryService.instance.clear();

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
        appBar: AppBar(title: const Text('Scan QR Code')),
        body: _isScanning
            ? MobileScanner(onDetect: _onDetect)
            : const Center(child: CircularProgressIndicator()),
      );
    }

    return Scaffold(
      appBar: AppBar(title: const Text('Kaptchi Start')),
      body: Row(
        children: [
          // Left side: Camera List
          Expanded(
            flex: 1,
            child: Container(
              color: Colors.black,
              child: Column(
                children: [
                  const Padding(
                    padding: EdgeInsets.all(16.0),
                    child: Text(
                      'Available Cameras',
                      style: TextStyle(
                        fontSize: 20,
                        fontWeight: FontWeight.bold,
                        color: Colors.white,
                      ),
                    ),
                  ),
                  Expanded(
                    child: _cameras.isEmpty
                        ? const Center(
                            child: Text(
                              'No cameras found',
                              style: TextStyle(color: Colors.white),
                            ),
                          )
                        : ListView.builder(
                            itemCount: _cameras.length,
                            itemBuilder: (context, index) {
                              final camera = _cameras[index];
                              final cleanName = camera.name
                                  .replaceAll(RegExp(r'<[^>]*>'), '')
                                  .trim();
                              return Card(
                                color: Colors.blue[900],
                                margin: const EdgeInsets.symmetric(
                                  horizontal: 16,
                                  vertical: 8,
                                ),
                                child: ListTile(
                                  leading: const Icon(
                                    Icons.camera_alt,
                                    color: Colors.white,
                                  ),
                                  title: Text(
                                    cleanName,
                                    style: const TextStyle(color: Colors.white),
                                  ),
                                  onTap: () {
                                    // Fix for swapped cameras when exactly 2 are present
                                    int targetIndex = index;
                                    if (_cameras.length == 2) {
                                      targetIndex = (index == 0) ? 1 : 0;
                                    }

                                    Navigator.push(
                                      context,
                                      MaterialPageRoute(
                                        builder: (_) => CameraScreen(
                                          initialCameraIndex: targetIndex,
                                        ),
                                      ),
                                    ).then((_) {
                                      if (mounted) {
                                        _startServer();
                                      }
                                    });
                                  },
                                ),
                              );
                            },
                          ),
                  ),
                  if (GalleryService.instance.images.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.all(16.0),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Divider(color: Colors.white24),
                          const Text(
                            'Export Settings',
                            style: TextStyle(
                              color: Colors.white,
                              fontWeight: FontWeight.bold,
                            ),
                          ),
                          const SizedBox(height: 8),
                          TextField(
                            controller: _pdfNameController,
                            style: const TextStyle(color: Colors.white),
                            onChanged: (val) {
                              // Local state only
                            },
                            decoration: const InputDecoration(
                              labelText: 'File Name',
                              labelStyle: TextStyle(color: Colors.white70),
                              enabledBorder: UnderlineInputBorder(
                                borderSide: BorderSide(color: Colors.white70),
                              ),
                            ),
                          ),
                          const SizedBox(height: 8),
                          Row(
                            children: [
                              Expanded(
                                child: TextField(
                                  controller: _pdfPathController,
                                  style: const TextStyle(
                                    color: Colors.white,
                                    fontSize: 12,
                                  ),
                                  readOnly: true,
                                  decoration: const InputDecoration(
                                    labelText: 'Save Location',
                                    labelStyle: TextStyle(
                                      color: Colors.white70,
                                    ),
                                    enabledBorder: UnderlineInputBorder(
                                      borderSide: BorderSide(
                                        color: Colors.white70,
                                      ),
                                    ),
                                  ),
                                ),
                              ),
                              IconButton(
                                icon: const Icon(
                                  Icons.folder_open,
                                  color: Colors.blue,
                                ),
                                onPressed: _pickSaveDirectory,
                              ),
                            ],
                          ),
                          const SizedBox(height: 16),
                          SizedBox(
                            width: double.infinity,
                            child: ElevatedButton.icon(
                              onPressed: _exportPdf,
                              icon: const Icon(Icons.picture_as_pdf),
                              label: Text(
                                'Export ${GalleryService.instance.images.length} Images to PDF',
                              ),
                              style: ElevatedButton.styleFrom(
                                backgroundColor: Colors.indigo,
                                foregroundColor: Colors.white,
                                padding: const EdgeInsets.symmetric(
                                  vertical: 16,
                                ),
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                ],
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
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    const SizedBox(height: 32), // Add some top padding
                    const Text(
                      'Connect Mobile Camera',
                      style: TextStyle(
                        fontSize: 24,
                        fontWeight: FontWeight.bold,
                        color: Colors.white,
                      ),
                    ),
                    const SizedBox(height: 16),

                    // Server IP Selection (Always Visible)
                    if (_availableInterfaces.isNotEmpty)
                      Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 64.0,
                          vertical: 8.0,
                        ),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              'Server Interface:',
                              style: TextStyle(
                                color: Colors.grey,
                                fontSize: 12,
                              ),
                            ),
                            DropdownButton<String>(
                              value: _serverIp,
                              isExpanded: true,
                              dropdownColor: Colors.grey[900],
                              style: const TextStyle(
                                color: Colors.white,
                                fontSize: 16,
                              ),
                              iconEnabledColor: Colors.white,
                              underline: Container(
                                height: 1,
                                color: Colors.white54,
                              ),
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
                                }
                              },
                            ),
                          ],
                        ),
                      ),

                    // Media Server Controls (Always Visible for configuration)
                    // Removed as per request

                    // RTMP QR Code
                    if (_serverIp.isNotEmpty) ...[
                      Container(
                        color: Colors.white,
                        padding: const EdgeInsets.all(16),
                        child: QrImageView(
                          data: 'rtmp://$_serverIp/live/stream',
                          version: QrVersions.auto,
                          size: 250.0,
                        ),
                      ),
                      const SizedBox(height: 16),
                      Text(
                        'Scan with Kaptchi mobile app',
                        style: TextStyle(fontSize: 16, color: Colors.grey[400]),
                      ),
                      const SizedBox(height: 16),

                      if (_isMediaServerRunning)
                        const Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(
                              Icons.check_circle,
                              color: Colors.green,
                              size: 16,
                            ),
                            SizedBox(width: 8),
                            Text(
                              'Media Server Running',
                              style: TextStyle(
                                color: Colors.green,
                                fontSize: 12,
                              ),
                            ),
                          ],
                        )
                      else
                        const Text(
                          'Media Server Stopped',
                          style: TextStyle(color: Colors.grey, fontSize: 12),
                          textAlign: TextAlign.center,
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
