import 'dart:convert';
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

typedef OnRemoteStream = void Function(MediaStream stream);

class WebRTCService {
  WebSocketChannel? _socket;
  RTCPeerConnection? _peerConnection;
  MediaStream? _localStream;
  MediaStream? _remoteStream;
  OnRemoteStream? onRemoteStream;
  Function(String)? onError;
  Function(RTCSignalingState)? onSignalingStateChange;
  Function(RTCIceConnectionState)? onIceConnectionStateChange;

  final Map<String, dynamic> _configuration = {
    'iceServers': [
      {'urls': 'stun:stun.l.google.com:19302'},
    ]
  };

  Future<void> connect(String hostOrUrl, bool isCaller) async {
    try {
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

      print('Connecting to WebSocket: $uri');
      _socket = WebSocketChannel.connect(uri);
      
      _socket!.stream.listen(
        (message) {
          _handleMessage(message);
        },
        onError: (error) {
          print('WebSocket error: $error');
          onError?.call('WebSocket error: $error');
        },
        onDone: () {
          print('WebSocket closed');
          onError?.call('WebSocket connection closed');
        },
      );

      _peerConnection = await createPeerConnection(_configuration);

      _peerConnection!.onSignalingState = (state) {
        print('Signaling state: $state');
        onSignalingStateChange?.call(state);
      };

      _peerConnection!.onIceConnectionState = (state) {
        print('ICE connection state: $state');
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
        _localStream = await navigator.mediaDevices.getUserMedia({
          'audio': false,
          'video': {
            'facingMode': 'environment', // Back camera
            'width': 3840,
            'height': 2160,
            'frameRate': 30,
          },
        });
        _localStream!.getTracks().forEach((track) {
          _peerConnection!.addTrack(track, _localStream!);
        });

        // Create Offer
        RTCSessionDescription offer = await _peerConnection!.createOffer();
        await _peerConnection!.setLocalDescription(offer);
        _send('offer', {'sdp': offer.sdp, 'type': offer.type});
      }
    } catch (e) {
      print('Error connecting: $e');
    }
  }

  void _handleMessage(dynamic message) async {
    try {
      if (message == null) return;
      
      Map<String, dynamic> data = jsonDecode(message);
      String? type = data['type'];
      var payload = data['payload'];

      if (type == null) {
        print('Received message without type');
        return;
      }

      switch (type) {
        case 'offer':
          await _peerConnection!.setRemoteDescription(
            RTCSessionDescription(payload['sdp'], payload['type']),
          );
          RTCSessionDescription answer = await _peerConnection!.createAnswer();
          await _peerConnection!.setLocalDescription(answer);
          _send('answer', {'sdp': answer.sdp, 'type': answer.type});
          break;

        case 'answer':
          await _peerConnection!.setRemoteDescription(
            RTCSessionDescription(payload['sdp'], payload['type']),
          );
          break;

        case 'candidate':
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
            if (command == 'zoom' && data != null) {
              double? level = data['level'];
              if (level != null) {
                setZoom(level);
              }
            }
          }
          break;
      }
    } catch (e) {
      print('Error handling message: $e');
    }
  }

  void _send(String type, Map<String, dynamic> payload) {
    if (_socket != null) {
      _socket!.sink.add(jsonEncode({
        'type': type,
        'payload': payload,
      }));
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
      print('Error disposing local stream: $e');
    }
    _localStream = null;
    
    // _remoteStream is managed by the peer connection usually, but we can null it
    _remoteStream = null;
    
    try {
      if (_peerConnection != null) {
        await _peerConnection!.close();
      }
    } catch (e) {
      print('Error closing peer connection: $e');
    }
    _peerConnection = null;
    
    try {
      _socket?.sink.close();
    } catch (e) {
      print('Error closing socket: $e');
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
