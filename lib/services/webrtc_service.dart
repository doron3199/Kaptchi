import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

import 'camera_owner_coordinator.dart';

typedef OnRemoteStream = void Function(MediaStream stream);

class WebRTCService {
  WebSocketChannel? _socket;
  RTCPeerConnection? _peerConnection;
  MediaStream? _localStream;
  MediaStream? _remoteStream;
  bool _cameraLeaseHeld = false;
  OnRemoteStream? onRemoteStream;
  Function(String)? onError;
  Function(RTCSignalingState)? onSignalingStateChange;
  Function(RTCIceConnectionState)? onIceConnectionStateChange;
  Function(String command, Map<String, dynamic> data)? onControlMessage;

  final Map<String, dynamic> _configuration = {
    'iceServers': [
      {'urls': 'stun:stun.l.google.com:19302'},
    ]
  };

  bool _isConnected = false;
  String? _lastUrl;
  
  bool get isConnected => _isConnected;

  Future<void> connect(String hostOrUrl, bool isCaller, {bool useWebRTC = true}) async {
    debugPrint('WebRTCService: connect called with $hostOrUrl, useWebRTC: $useWebRTC');
    try {
      _lastUrl = hostOrUrl;
      String cleanHostOrUrl = hostOrUrl.trim();
      Uri uri;
      if (cleanHostOrUrl.startsWith('ws://') || cleanHostOrUrl.startsWith('wss://')) {
        uri = Uri.parse(cleanHostOrUrl);
      } else {
        // Default to port 5000 if not specified
        if (cleanHostOrUrl.contains(':')) {
           uri = Uri.parse('ws://$cleanHostOrUrl');
        } else {
           uri = Uri.parse('ws://$cleanHostOrUrl:5000');
        }
      }

      debugPrint('Connecting to WebSocket: $uri');
      _socket = WebSocketChannel.connect(uri);
      _isConnected = true;
      
      _socket!.stream.listen(
        (message) {
          _handleMessage(message);
        },
        onError: (error) {
          debugPrint('WebSocket error: $error');
          _isConnected = false;
          onError?.call('WebSocket error: $error');
          _socket = null;
        },
        onDone: () {
          debugPrint('WebSocket closed');
          _isConnected = false;
          onError?.call('WebSocket connection closed');
          _socket = null;
        },
      );

      if (!useWebRTC) {
        debugPrint('WebRTCService: Skipping PeerConnection creation (Signaling only mode)');
        return;
      }

      // Only create peer connection if we intend to use WebRTC media
      _peerConnection = await createPeerConnection(_configuration);

      _peerConnection!.onSignalingState = (state) {
        debugPrint('Signaling state: $state');
        onSignalingStateChange?.call(state);
      };

      _peerConnection!.onIceConnectionState = (state) {
        debugPrint('ICE connection state: $state');
        onIceConnectionStateChange?.call(state);
      };

      _peerConnection!.onIceCandidate = (candidate) {
        _send('candidate', {
          'candidate': candidate.candidate,
          'sdpMid': candidate.sdpMid,
          'sdpMLineIndex': candidate.sdpMLineIndex,
        });
      };

      _peerConnection!.onTrack = (event) {
        if (event.streams.isNotEmpty) {
          _remoteStream = event.streams[0];
          if (onRemoteStream != null) {
            onRemoteStream!(_remoteStream!);
          }
        }
      };

      // If we are the caller (Android Camera), we need to add our local stream
      if (isCaller) {
        debugPrint('Requesting local stream...');
        try {
          await CameraOwnerCoordinator.instance.acquire('webrtc');
          _cameraLeaseHeld = true;

          _localStream = await navigator.mediaDevices.getUserMedia({
            'audio': false,
            'video': {
              'facingMode': 'environment', // Back camera
              'width': 1280, // Lowered from 3840 to prevent encoder crash
              'height': 720, // Lowered from 2160 to prevent encoder crash
              'frameRate': 30,
            },
          });
          debugPrint('Local stream obtained');
        } catch (e) {
          debugPrint('Error getting user media: $e');
          if (_cameraLeaseHeld) {
            CameraOwnerCoordinator.instance.release('webrtc');
            _cameraLeaseHeld = false;
          }
          throw e;
        }
        
        _localStream!.getTracks().forEach((track) {
          _peerConnection!.addTrack(track, _localStream!);
        });

        // Create Offer
        RTCSessionDescription offer = await _peerConnection!.createOffer();
        await _peerConnection!.setLocalDescription(offer);
        _send('offer', {'sdp': offer.sdp, 'type': offer.type});
      }
    } catch (e) {
      debugPrint('Error connecting: $e');
    }
  }

  void _handleMessage(dynamic message) async {
    try {
      if (message == null) return;
      debugPrint('WebRTCService received: $message');
      
      Map<String, dynamic> data = jsonDecode(message);
      String? type = data['type'];
      var payload = data['payload'];

      if (type == null) {
        debugPrint('Received message without type');
        return;
      }

      if (_peerConnection == null) {
        // If peer connection is null, we might still want to handle control messages
        if (type == 'control') {
           if (payload != null) {
            String? command = payload['command'];
            var data = payload['data'];
            if (command != null && data != null) {
              onControlMessage?.call(command, data);
            }
          }
          return;
        }
        debugPrint('PeerConnection is null, ignoring message type: $type');
        return;
      }

      switch (type) {
        case 'offer':
          debugPrint('Received offer');
          await _peerConnection!.setRemoteDescription(
            RTCSessionDescription(payload['sdp'], payload['type']),
          );
          RTCSessionDescription answer = await _peerConnection!.createAnswer();
          await _peerConnection!.setLocalDescription(answer);
          _send('answer', {'sdp': answer.sdp, 'type': answer.type});
          break;

        case 'answer':
          debugPrint('Received answer');
          await _peerConnection!.setRemoteDescription(
            RTCSessionDescription(payload['sdp'], payload['type']),
          );
          break;

        case 'candidate':
          debugPrint('Received candidate');
          if (payload != null) {
            await _peerConnection!.addCandidate(
              RTCIceCandidate(
                payload['candidate'],
                payload['sdpMid'],
                payload['sdpMLineIndex'],
              ),
            );
          }
          break;
          
        case 'control':
          if (payload != null) {
            String? command = payload['command'];
            var data = payload['data'];
            if (command != null && data != null) {
              // Notify listeners
              onControlMessage?.call(command, data);

              if (command == 'zoom') {
                double? level = data['level'];
                if (level != null) {
                  setZoom(level);
                }
              }
            }
          }
          break;
      }
    } catch (e) {
      debugPrint('Error handling message: $e');
    }
  }

  void _send(String type, Map<String, dynamic> payload) async {
    if (_socket == null) {
       debugPrint('WebRTCService: Socket is null. _lastUrl: $_lastUrl');
       if (_lastUrl != null) {
         debugPrint('WebRTCService: Attempting to reconnect to $_lastUrl...');
         await connect(_lastUrl!, false, useWebRTC: false);
       }
    }

    if (_socket != null) {
      try {
        final msg = jsonEncode({
          'type': type,
          'payload': payload,
        });
        debugPrint('WebRTCService sending: $msg');
        _socket!.sink.add(msg);
      } catch (e) {
        debugPrint('Error sending message: $e');
      }
    } else {
      debugPrint('WebRTCService: Cannot send message, socket is null');
    }
  }

  void sendControlMessage(String command, Map<String, dynamic> data) {
    _send('control', {
      'command': command,
      'data': data,
    });
  }

  Future<void> disconnect() async {
    try {
      await _localStream?.dispose();
    } catch (e) {
      debugPrint('Error disposing local stream: $e');
    }
    if (_cameraLeaseHeld) {
      CameraOwnerCoordinator.instance.release('webrtc');
      _cameraLeaseHeld = false;
    }
    _localStream = null;
    
    // _remoteStream is managed by the peer connection usually, but we can null it
    _remoteStream = null;
    
    try {
      if (_peerConnection != null) {
        await _peerConnection!.close();
      }
    } catch (e) {
      debugPrint('Error closing peer connection: $e');
    }
    _peerConnection = null;
    
    try {
      _socket?.sink.close();
    } catch (e) {
      debugPrint('Error closing socket: $e');
    }
    _socket = null;
  }
  
  MediaStream? get localStream => _localStream;
  MediaStream? get remoteStream => _remoteStream;

  Future<void> setZoom(double zoomLevel) async {
    if (_localStream == null) return;
    var videoTrack = _localStream!.getVideoTracks().first;
    await Helper.setZoom(videoTrack, zoomLevel);
  }
}
