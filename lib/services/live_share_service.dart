import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'app_logger.dart';
import 'native_camera_service.dart';

class LiveShareService {
  static final LiveShareService _instance = LiveShareService._internal();
  factory LiveShareService() => _instance;
  LiveShareService._internal();

  final NativeCameraService _cameraService = NativeCameraService();
  WebSocket? _socket;
  Timer? _pollingTimer;
  int _lastCanvasVersion = -1;
  bool _isConnecting = false;
  
  String? _currentBoardId;

  /// Starts broadcasting the live whiteboard canvas to the Next.js target server
  Future<void> startBroadcasting(String wsUrl, String boardId) async {
    if (_socket != null || _isConnecting) return;
    
    _isConnecting = true;
    _currentBoardId = boardId;
    AppLogger.network('LiveShareService: Connecting to WS relay $wsUrl for board $boardId');
    
    try {
      _socket = await WebSocket.connect(wsUrl);
      AppLogger.network('LiveShareService: Connected successfully!');
      
      _socket!.listen(
        (message) {
          // Typically we just send from app to web, but if we receive commands we handle here
        },
        onDone: () {
          AppLogger.network('LiveShareService: Socket closed.');
          stopBroadcasting();
        },
        onError: (err) {
          AppLogger.network('LiveShareService: Socket error $err');
          stopBroadcasting();
        },
      );
      
      _isConnecting = false;

      // Start polling the canvas
      _startPolling();

    } catch (e) {
      AppLogger.network('LiveShareService: Failed to connect $e');
      _isConnecting = false;
    }
  }

  void _startPolling() {
    _pollingTimer?.cancel();
    
    // Poll continuously at 1 FPS (every 1000ms) right now to ensure the web updates unconditionally.
    _pollingTimer = Timer.periodic(const Duration(milliseconds: 1000), (timer) {
      if (_socket == null || _socket!.readyState != WebSocket.open) {
        timer.cancel();
        return;
      }
      
      // Fetch JPEG string directly from true canvas overview 
      final jpegBytes = _cameraService.getCanvasOverviewJpeg(8192, quality: 100);
      
      if (jpegBytes != null && jpegBytes.isNotEmpty) {
        // Send over websocket
        try {
           _socket!.add(jpegBytes);
        } catch (e) {
           AppLogger.network('LiveShareService: Error sending data $e');
        }
      }
    });
  }

  void stopBroadcasting() {
    _pollingTimer?.cancel();
    _pollingTimer = null;
    
    if (_socket != null) {
      _socket?.close();
      _socket = null;
    }
    
    _currentBoardId = null;
    _isConnecting = false;
    AppLogger.network('LiveShareService: Stopped broadcasting.');
  }

  bool get isBroadcasting => _socket != null && _socket!.readyState == WebSocket.open;
  String? get currentBoardId => _currentBoardId;
}
