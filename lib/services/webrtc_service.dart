import 'dart:convert';
import 'package:flutter_webrtc/flutter_webrtc.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

typedef OnRemoteStream = void Function(MediaStream stream);

class WebRTCService {
  WebSocketChannel? _socket;
  RTCPeerConnection? _peerConnection;
  MediaStream? _localStream;
  OnRemoteStream? onRemoteStream;

  final Map<String, dynamic> _configuration = {
    'iceServers': [
      {'urls': 'stun:stun.l.google.com:19302'},
    ]
  };

  Future<void> connect(String ipAddress, bool isCaller) async {
    try {
      _socket = WebSocketChannel.connect(Uri.parse('ws://$ipAddress:8080'));
      
      _socket!.stream.listen((message) {
        _handleMessage(message);
      });

      _peerConnection = await createPeerConnection(_configuration);

      _peerConnection!.onIceCandidate = (candidate) {
        _send('candidate', {
          'candidate': candidate.candidate,
          'sdpMid': candidate.sdpMid,
          'sdpMLineIndex': candidate.sdpMLineIndex,
        });
      };

      _peerConnection!.onTrack = (event) {
        if (event.streams.isNotEmpty && onRemoteStream != null) {
          onRemoteStream!(event.streams[0]);
        }
      };

      // If we are the caller (Android Camera), we need to add our local stream
      if (isCaller) {
        _localStream = await navigator.mediaDevices.getUserMedia({
          'audio': false,
          'video': {
            'facingMode': 'environment', // Back camera
            'width': 1280,
            'height': 720,
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
    Map<String, dynamic> data = jsonDecode(message);
    String type = data['type'];
    var payload = data['payload'];

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
        await _peerConnection!.addCandidate(
          RTCIceCandidate(
            payload['candidate'],
            payload['sdpMid'],
            payload['sdpMLineIndex'],
          ),
        );
        break;
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

  Future<void> disconnect() async {
    _localStream?.dispose();
    _peerConnection?.close();
    _socket?.sink.close();
  }
  
  MediaStream? get localStream => _localStream;
}
