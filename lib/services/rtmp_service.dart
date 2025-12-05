import 'dart:io';
import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:haishin_kit/audio_source.dart';
import 'package:haishin_kit/rtmp_connection.dart';
import 'package:haishin_kit/rtmp_stream.dart';
import 'package:haishin_kit/video_source.dart';
import 'package:permission_handler/permission_handler.dart';

class RtmpService {
  static final RtmpService _instance = RtmpService._internal();
  static RtmpService get instance => _instance;

  RtmpConnection? _connection;
  RtmpStream? _stream;
  final ValueNotifier<bool> isStreamingNotifier = ValueNotifier<bool>(false);
  final ValueNotifier<RtmpStream?> streamNotifier = ValueNotifier<RtmpStream?>(null);
  
  // Zoom state
  // double _currentZoom = 1.0; // Not supported in this version
  // double _maxZoom = 10.0;

  RtmpService._internal();

  bool get isStreaming => isStreamingNotifier.value;
  RtmpStream? get stream => _stream;

  Future<void> initialize() async {
    if (!Platform.isAndroid && !Platform.isIOS) return;

    // Request permissions
    await [
      Permission.camera,
      Permission.microphone,
    ].request();

    _connection = await RtmpConnection.create();
    _stream = await RtmpStream.create(_connection!);
    
    // Configure Audio
    _stream?.audioSettings.bitrate = 64 * 1000;
    await _stream?.attachAudio(AudioSource());

    // Configure Video
    _stream?.videoSettings.width = 1280;
    _stream?.videoSettings.height = 720;
    _stream?.videoSettings.bitrate = 2500 * 1000;
    
    // Attach Camera
    _videoSource = VideoSource(position: CameraPosition.back);
    await _stream?.attachVideo(_videoSource);

    streamNotifier.value = _stream;

    // Listen for connection events
    _connection?.eventChannel.receiveBroadcastStream().listen((event) {
      final data = event["data"];
      if (data == null) return;
      
      switch (data["code"]) {
        case 'NetConnection.Connect.Success':
          debugPrint("RTMP Connected successfully!");
          _stream?.publish(_streamName);
          isStreamingNotifier.value = true;
          break;
        case 'NetConnection.Connect.Closed':
        case 'NetConnection.Connect.Failed':
           debugPrint("RTMP Connection failed or closed: ${data['code']}");
           isStreamingNotifier.value = false;
           break;
      }
    });
  }

  VideoSource? _videoSource;

  String _streamName = 'live';

  Future<void> startStream(String url) async {
    if (_connection == null) await initialize();
    
    String connectUrl = url;
    final uri = Uri.parse(url);
    if (uri.pathSegments.length > 1) {
      // Assume last segment is stream name
      _streamName = uri.pathSegments.last;
      // Reconstruct URL without the stream name
      final newPath = uri.pathSegments.sublist(0, uri.pathSegments.length - 1).join('/');
      connectUrl = uri.replace(path: newPath).toString();
    }
    
    try {
      _connection?.connect(connectUrl);
      // Connection status is handled by the event listener
    } catch (e) {
      debugPrint('Error starting RTMP stream: $e');
    }
  }

  Future<void> stopStream() async {
    _stream?.close();
    _connection?.close();
    isStreamingNotifier.value = false;
    streamNotifier.value = null;
    _stream = null;
    _connection = null;
  }

  void setZoom(double zoom) {
    // _currentZoom = zoom.clamp(1.0, _maxZoom);
    // Zoom is not currently supported by the haishin_kit 0.14.3 Dart API
  }
  
  void dispose() {
    _stream?.dispose();
    _connection?.dispose();
  }
}
