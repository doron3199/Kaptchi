import 'package:flutter/material.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:qr_flutter/qr_flutter.dart';
import '../services/signaling_server.dart';
import '../services/webrtc_service.dart';
import '../services/image_processing_service.dart';

class MonitorScreen extends StatefulWidget {
  const MonitorScreen({super.key});

  @override
  State<MonitorScreen> createState() => _MonitorScreenState();
}

class _MonitorScreenState extends State<MonitorScreen> {
  final _signalingServer = SignalingServer();
  final _webrtcService = WebRTCService();
  final _remoteRenderer = RTCVideoRenderer();
  
  List<({String name, String ip})> _availableInterfaces = [];
  int _currentIpIndex = 0;
  int _serverPort = 5000;
  
  String get _serverIp => _availableInterfaces.isNotEmpty ? _availableInterfaces[_currentIpIndex].ip : 'Unknown IP';
  String get _interfaceName => _availableInterfaces.isNotEmpty ? _availableInterfaces[_currentIpIndex].name : '';

  String _errorMessage = '';

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    try {
      await _remoteRenderer.initialize();
      
      try {
        await _signalingServer.start();
        setState(() {
          _serverPort = _signalingServer.port;
        });
      } catch (e) {
        print('Error starting signaling server: $e');
        setState(() {
          _errorMessage = 'Server Error: $e';
        });
      }
      
      // Initialize C++ processing
      try {
        await ImageProcessingService.instance.initialize();
      } catch (e) {
        print('Error initializing image processing: $e');
      }

      final interfaces = await _signalingServer.getNetworkInterfaces();
      setState(() {
        _availableInterfaces = interfaces;
      });

      // Connect to our own local server to listen for the camera
      await _webrtcService.connect('localhost:$_serverPort', false);
      
      _webrtcService.onRemoteStream = (stream) {
        setState(() {
          _remoteRenderer.srcObject = stream;
        });
        
        // Attach C++ processor to the incoming stream
        if (stream.getVideoTracks().isNotEmpty) {
          ImageProcessingService.instance.attachToTrack(stream.getVideoTracks().first);
        }
      };
    } catch (e) {
      print('Fatal error in _init: $e');
      setState(() {
        _errorMessage = 'Init Error: $e';
      });
    }
  }

  void _cycleIp() {
    if (_availableInterfaces.length > 1) {
      setState(() {
        _currentIpIndex = (_currentIpIndex + 1) % _availableInterfaces.length;
      });
    }
  }

  @override
  void dispose() {
    _remoteRenderer.dispose();
    _webrtcService.disconnect();
    _signalingServer.stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text('Monitor Mode (IP: $_serverIp)')),
      body: Center(
        child: _remoteRenderer.srcObject != null
            ? RTCVideoView(_remoteRenderer)
            : Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  const Text('Waiting for camera connection...', style: TextStyle(fontSize: 18)),
                  const SizedBox(height: 20),
                  if (_availableInterfaces.isNotEmpty)
                    GestureDetector(
                      onTap: _cycleIp,
                      child: Column(
                        children: [
                          Container(
                            padding: const EdgeInsets.all(16),
                            color: Colors.white,
                            child: QrImageView(
                              data: 'ws://$_serverIp:$_serverPort',
                              version: QrVersions.auto,
                              size: 200.0,
                            ),
                          ),
                          const SizedBox(height: 8),
                          Row(
                            mainAxisAlignment: MainAxisAlignment.center,
                            children: [
                              Text(
                                'IP: $_serverIp:$_serverPort ($_interfaceName)',
                                style: const TextStyle(fontWeight: FontWeight.bold),
                              ),
                              IconButton(
                                icon: const Icon(Icons.refresh),
                                onPressed: () async {
                                  final interfaces = await _signalingServer.getNetworkInterfaces();
                                  setState(() {
                                    _availableInterfaces = interfaces;
                                    _currentIpIndex = 0;
                                  });
                                },
                              ),
                            ],
                          ),
                          if (_availableInterfaces.length > 1)
                            Text(
                              'Tap QR to switch IP (${_currentIpIndex + 1}/${_availableInterfaces.length})',
                              style: const TextStyle(color: Colors.grey),
                            ),
                        ],
                      ),
                    )
                  else
                    const Text('No network interfaces found'),
                  const SizedBox(height: 10),
                  const Text('Scan this QR code with the mobile app to connect'),
                  if (_errorMessage.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.all(8.0),
                      child: Text(_errorMessage, style: const TextStyle(color: Colors.red)),
                    ),
                ],
              ),
      ),
    );
  }
}
