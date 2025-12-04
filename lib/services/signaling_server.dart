import 'dart:async';
import 'dart:io';
import 'package:shelf/shelf_io.dart' as shelf_io;
import 'package:shelf_web_socket/shelf_web_socket.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

class SignalingServer {
  static final SignalingServer instance = SignalingServer._internal();
  
  factory SignalingServer() {
    return instance;
  }
  
  SignalingServer._internal();

  HttpServer? _server;
  final List<WebSocketChannel> _clients = [];
  
  int get port => _server?.port ?? 5000;
  bool get isRunning => _server != null;

  Future<void> start() async {
    if (_server != null) {
      print('Signaling server already running on port ${_server!.port}');
      return;
    }

    var handler = webSocketHandler((WebSocketChannel webSocket, String? protocol) {
      _clients.add(webSocket);
      print('Client connected. Total clients: ${_clients.length}');

      webSocket.stream.listen((message) {
        // Broadcast message to all other clients
        for (var client in _clients) {
          if (client != webSocket) {
            try {
              client.sink.add(message);
            } catch (e) {
              print('Error broadcasting to client: $e');
            }
          }
        }
      }, onDone: () {
        _clients.remove(webSocket);
        print('Client disconnected. Total clients: ${_clients.length}');
      });
    });

    // Listen on all interfaces (0.0.0.0) so external devices can connect
    // Try a list of specific ports. Do NOT use random ports to avoid firewall issues.
    // Prioritize 5000 as it is the default in WebRTCService
    final ports = [5000, 8080, 5001, 8081, 3000, 8888];
    
    for (final port in ports) {
      try {
        _server = await shelf_io.serve(handler, InternetAddress.anyIPv4, port);
        print('Signaling server running on ws://${_server!.address.address}:${_server!.port}');
        return; // Success
      } catch (e) {
        print('Port $port busy, trying next...');
      }
    }
    
    // If we get here, all ports failed
    throw Exception('Could not bind to any port: $ports. Please close other instances of the app.');
  }

  Future<void> stop() async {
    try {
      await _server?.close(force: true);
    } catch (e) {
      print('Error closing server: $e');
    } finally {
      _server = null;
    }
    
    // Create a copy to iterate over, because closing might trigger onDone which modifies _clients
    final clientsCopy = List<WebSocketChannel>.from(_clients);
    for (var client in clientsCopy) {
      try {
        await client.sink.close();
      } catch (e) {
        print('Error closing client sink: $e');
      }
    }
    _clients.clear();
  }
  
  // Helper to get local IP to show in UI
  Future<List<({String name, String ip})>> getNetworkInterfaces() async {
    final List<({String name, String ip})> interfacesList = [];
    try {
      print('DEBUG: Listing network interfaces...');
      final interfaces = await NetworkInterface.list(
        includeLoopback: true, 
        includeLinkLocal: true,
        type: InternetAddressType.IPv4
      );

      for (var interface in interfaces) {
        print('DEBUG: Found interface: ${interface.name}');
        for (var addr in interface.addresses) {
          print('DEBUG:   Address: ${addr.address}');
          if (addr.type == InternetAddressType.IPv4) {
             // Filter out link-local (APIPA) addresses which are usually useless for this
             if (!addr.address.startsWith('169.254')) {
                interfacesList.add((name: interface.name, ip: addr.address));
             } else {
                print('DEBUG:   Skipping link-local address: ${addr.address}');
             }
          }
        }
      }
    } catch (e) {
      print('Error getting IPs: $e');
    }
    
    if (interfacesList.isEmpty) {
      interfacesList.add((name: 'Loopback', ip: '127.0.0.1'));
    }
    
    // Sort: Wi-Fi first, then Ethernet, then others. Also prioritize private ranges.
    interfacesList.sort((a, b) {
      // Helper to score IP ranges
      int score(String ip) {
        if (ip.startsWith('192.168.')) return 3;
        if (ip.startsWith('10.')) return 2;
        if (ip.startsWith('172.')) return 1;
        return 0;
      }
      
      final scoreA = score(a.ip);
      final scoreB = score(b.ip);
      if (scoreA > scoreB) return -1;
      if (scoreB > scoreA) return 1;

      final nameA = a.name.toLowerCase();
      final nameB = b.name.toLowerCase();
      
      bool isWifi(String name) => name.contains('wi-fi') || name.contains('wlan') || name.contains('wireless');

      // Prioritize Wi-Fi
      if (isWifi(nameA) && !isWifi(nameB)) return -1;
      if (!isWifi(nameA) && isWifi(nameB)) return 1;
      // Then Ethernet
      if (nameA.contains('ethernet') && !nameB.contains('ethernet')) return -1;
      if (!nameA.contains('ethernet') && nameB.contains('ethernet')) return 1;
      return 0;
    });
    
    return interfacesList;
  }
}
