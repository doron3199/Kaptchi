import 'dart:async';
import 'dart:io';
import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';
import 'package:camera/camera.dart';
import 'package:qr_flutter/qr_flutter.dart';
import '../services/signaling_server.dart';
import '../services/webrtc_service.dart';
import '../models/stream_protocol.dart';
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
  SignalingServer? _signalingServer;
  WebRTCService? _webrtcService;
  String _serverIp = '';
  int _serverPort = 5000;
  bool _isServerRunning = false;
  bool _isMediaServerRunning = false;
  String? _mediaServerPath; // Add this
  List<({String name, String ip})> _availableInterfaces = [];
  
  // Protocol Selection
  // StreamProtocol _selectedProtocol = StreamProtocol.rtmp; // Unused
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
        
        _navigateToCamera(StreamProtocol.rtmp, url);
      });
    }
  }

  @override
  void dispose() {
    _streamSubscription?.cancel();
    _stopServer();
    MediaServerService.instance.stopServer();
    super.dispose();
  }

  void _navigateToCamera(StreamProtocol protocol, String url) {
     _webrtcService?.disconnect();
     _webrtcService = null;
     
     Navigator.push(
       context,
       MaterialPageRoute(
         builder: (_) => CameraScreen(
           initialProtocol: protocol,
           initialStreamUrl: url,
         ),
       ),
     ).then((_) {
       if (mounted) {
         _startServer();
         // Reset to WebRTC when coming back
         setState(() {
           // _selectedProtocol = StreamProtocol.webrtc;
         });
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
      debugPrint('Error starting server: $e');
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
                                    // _stopServer(); // Don't stop the server!
                                    // We might want to disconnect the local WebRTC client though?
                                    // If we don't, and a call comes in, we might get navigated away.
                                    // For now, let's just disconnect the client but keep server.
                                    _webrtcService?.disconnect();
                                    _webrtcService = null;
                                    
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
