import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:kaptchi_flutter/services/raw_socket_service.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:camera/camera.dart';
import 'package:qr_flutter/qr_flutter.dart';
import '../services/media_server_service.dart'; 
import 'camera_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  bool _isScanning = true;
  
  // Windows State
  List<CameraDescription> _cameras = [];
  String _serverIp = '';
  bool _isMediaServerRunning = false;
  String? _mediaServerPath; // Add this
  List<({String name, String ip})> _availableInterfaces = [];
  
  // Protocol Selection
  StreamSubscription? _streamSubscription;

  @override
  void initState() {
    super.initState();
    if (Platform.isWindows && !Platform.environment.containsKey('FLUTTER_TEST')) {
      _loadCameras();
      _startServer();
      // Don't auto-start media server, let user choose or try default if they want
      _startMediaServer(); 
      
      // Listen for RTMP stream start
      _streamSubscription = MediaServerService.instance.onStreamStarted.listen((path) {
        if (!mounted) return;
        // Always navigate if a stream starts, assuming user wants to see it
        // Construct the full URL
        final url = 'rtmp://localhost/$path';
        
        _navigateToCamera(url);
      });
    }
  }

  @override
  void dispose() {
    _streamSubscription?.cancel();
    MediaServerService.instance.stopServer();
    super.dispose();
  }

  void _navigateToCamera(String url) {

     Navigator.push(
       context,
       MaterialPageRoute(
         builder: (_) => CameraScreen(
           initialStreamUrl: url,
         ),
       ),
     ).then((_) {
       if (mounted) {
         _startServer();

       }
     });
  }

  Future<void> _startMediaServer() async {
    final success = await MediaServerService.instance.startServer(executablePath: _mediaServerPath);
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
                    child: Text('Available Cameras', style: TextStyle(fontSize: 20, fontWeight: FontWeight.bold, color: Colors.white)),
                  ),
                  Expanded(
                    child: _cameras.isEmpty
                        ? const Center(child: Text('No cameras found', style: TextStyle(color: Colors.white)))
                        : ListView.builder(
                            itemCount: _cameras.length,
                            itemBuilder: (context, index) {
                              final camera = _cameras[index];
                              final cleanName = camera.name.replaceAll(RegExp(r'<[^>]*>'), '').trim();
                              return Card(
                                color: Colors.blue[900],
                                margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                                child: ListTile(
                                  leading: const Icon(Icons.camera_alt, color: Colors.white),
                                  title: Text(cleanName, style: const TextStyle(color: Colors.white)),
                                  onTap: () {
                                    // Fix for swapped cameras when exactly 2 are present
                                    int targetIndex = index;
                                    if (_cameras.length == 2) {
                                       targetIndex = (index == 0) ? 1 : 0;
                                    }

                                    Navigator.push(
                                      context,
                                      MaterialPageRoute(
                                        builder: (_) => CameraScreen(initialCameraIndex: targetIndex),
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
                    const Text('Connect Mobile Camera', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: Colors.white)),
                    const SizedBox(height: 16),

                  // Server IP Selection (Always Visible)
                  if (_availableInterfaces.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 64.0, vertical: 8.0),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text('Server Interface:', style: TextStyle(color: Colors.grey, fontSize: 12)),
                          DropdownButton<String>(
                            value: _serverIp,
                            isExpanded: true,
                            dropdownColor: Colors.grey[900],
                            style: const TextStyle(color: Colors.white, fontSize: 16),
                            iconEnabledColor: Colors.white,
                            underline: Container(height: 1, color: Colors.white54),
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
                      Text('Scan with Kaptchi mobile app', style: TextStyle(fontSize: 16, color: Colors.grey[400])),
                      const SizedBox(height: 16),
                      
                      if (_isMediaServerRunning)
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
                          style: TextStyle(color: Colors.grey, fontSize: 12),
                          textAlign: TextAlign.center,
                        ),
                     ]
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
