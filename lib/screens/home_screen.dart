import 'dart:io';
import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:camera/camera.dart';
import 'package:qr_flutter/qr_flutter.dart';
import '../services/signaling_server.dart';
import '../services/webrtc_service.dart';
import 'camera_screen.dart';
// import 'monitor_screen.dart'; // Removed as requested

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  bool _isScanning = true;
  
  // Windows State
  List<CameraDescription> _cameras = [];
  SignalingServer? _signalingServer;
  WebRTCService? _webrtcService;
  String _serverIp = '';
  int _serverPort = 5000;
  bool _isServerRunning = false;
  List<({String name, String ip})> _availableInterfaces = [];

  @override
  void initState() {
    super.initState();
    if (Platform.isWindows) {
      _loadCameras();
      _startServer();
    }
  }

  @override
  void dispose() {
    _stopServer();
    super.dispose();
  }

  Future<void> _loadCameras() async {
    try {
      _cameras = await availableCameras();
      if (mounted) setState(() {});
    } catch (e) {
      print('Error loading cameras: $e');
    }
  }

  Future<void> _startServer() async {
    _signalingServer = SignalingServer.instance;
    _webrtcService = WebRTCService();

    try {
      await _signalingServer!.start();
      final interfaces = await _signalingServer!.getNetworkInterfaces();
      
      if (mounted) {
        setState(() {
          _serverPort = _signalingServer!.port;
          _availableInterfaces = interfaces;
          if (interfaces.isNotEmpty) {
            _serverIp = interfaces[0].ip;
          }
          _isServerRunning = true;
        });
      }

      // Connect WebRTC Service to local signaling server
      await _webrtcService!.connect('localhost:$_serverPort', false);
      
      _webrtcService!.onRemoteStream = (stream) {
        if (!mounted) return;
        
        // Navigate to CameraScreen in WebRTC mode
        // We pass the services, so CameraScreen takes ownership
        final service = _webrtcService;
        // We don't pass the server anymore as it is a singleton, 
        // but we keep the parameter for compatibility or remove it.
        // Let's keep passing it but CameraScreen should know not to stop it.
        final server = _signalingServer;
        
        // We do NOT nullify _webrtcService here because we want it to stay alive?
        // Actually, CameraScreen takes ownership of the stream.
        // If we come back, we might need to recreate WebRTCService or reconnect.
        // For now, let's stick to the existing flow but WITHOUT stopping the server.
        
        _webrtcService = null;
        _signalingServer = null;
        // _isServerRunning = false; // Server is still running!

        Navigator.push(
          context,
          MaterialPageRoute(
            builder: (_) => CameraScreen(
              preConnectedWebRTCService: service,
              preStartedSignalingServer: server,
            ),
          ),
        ).then((_) {
           if (mounted) {
             _startServer();
           }
        });
      };

    } catch (e) {
      print('Error starting server: $e');
      if (mounted) {
        setState(() {
          _isServerRunning = false;
        });
      }
    }
  }

  void _stopServer() {
    _webrtcService?.disconnect();
    // _signalingServer?.stop(); // NEVER STOP THE SERVER
    _webrtcService = null;
    _signalingServer = null;
    // _isServerRunning = false; // Server is still running
  }

  void _onDetect(BarcodeCapture capture) {
    if (!_isScanning) return;
    
    final List<Barcode> barcodes = capture.barcodes;
    for (final barcode in barcodes) {
      if (barcode.rawValue != null) {
        setState(() {
          _isScanning = false;
        });
        
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
                                    // _stopServer(); // Don't stop the server!
                                    // We might want to disconnect the local WebRTC client though?
                                    // If we don't, and a call comes in, we might get navigated away.
                                    // For now, let's just disconnect the client but keep server.
                                    _webrtcService?.disconnect();
                                    _webrtcService = null;
                                    
                                    Navigator.push(
                                      context,
                                      MaterialPageRoute(
                                        builder: (_) => CameraScreen(initialCameraIndex: index),
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
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  const Text('Connect Mobile Camera', style: TextStyle(fontSize: 24, fontWeight: FontWeight.bold, color: Colors.white)),
                  const SizedBox(height: 32),
                  if (_isServerRunning && _serverIp.isNotEmpty) ...[
                    Container(
                      color: Colors.white,
                      padding: const EdgeInsets.all(16),
                      child: QrImageView(
                        data: 'ws://$_serverIp:$_serverPort',
                        version: QrVersions.auto,
                        size: 250.0,
                      ),
                    ),
                    const SizedBox(height: 16),
                    Text('Scan with Kaptchi mobile app', style: TextStyle(fontSize: 16, color: Colors.grey[400])),
                    const SizedBox(height: 8),
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 64.0),
                      child: DropdownButton<String>(
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
                    ),
                    const SizedBox(height: 8),
                    Text('ws://$_serverIp:$_serverPort', style: const TextStyle(fontSize: 14, fontWeight: FontWeight.bold, color: Colors.white)),
                  ] else ...[
                    const CircularProgressIndicator(color: Colors.white),
                    const SizedBox(height: 16),
                    const Text('Starting server...', style: TextStyle(color: Colors.white)),
                  ],
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}
