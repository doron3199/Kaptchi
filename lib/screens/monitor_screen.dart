import 'package:flutter/material.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';
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
  String _serverIp = 'Starting...';

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    await _remoteRenderer.initialize();
    await _signalingServer.start();
    
    // Initialize C++ processing
    await ImageProcessingService.instance.initialize();

    String ip = await _signalingServer.getIpAddress();
    setState(() {
      _serverIp = ip;
    });

    // Connect to our own local server to listen for the camera
    await _webrtcService.connect('localhost', false);
    
    _webrtcService.onRemoteStream = (stream) {
      setState(() {
        _remoteRenderer.srcObject = stream;
      });
      
      // Attach C++ processor to the incoming stream
      if (stream.getVideoTracks().isNotEmpty) {
        ImageProcessingService.instance.attachToTrack(stream.getVideoTracks().first);
      }
    };
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
            : const Text('Waiting for camera connection...'),
      ),
    );
  }
}
