import 'dart:async';
import 'dart:io';
import 'package:shelf/shelf_io.dart' as shelf_io;
import 'package:shelf_web_socket/shelf_web_socket.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

class SignalingServer {
  HttpServer? _server;
  final List<WebSocketChannel> _clients = [];

  Future<void> start() async {
    var handler = webSocketHandler((WebSocketChannel webSocket) {
      _clients.add(webSocket);
      print('Client connected. Total clients: ${_clients.length}');

      webSocket.stream.listen((message) {
        // Broadcast message to all other clients
        for (var client in _clients) {
          if (client != webSocket) {
            client.sink.add(message);
          }
        }
      }, onDone: () {
        _clients.remove(webSocket);
        print('Client disconnected. Total clients: ${_clients.length}');
      });
    });

    // Listen on all interfaces (0.0.0.0) so external devices can connect
    _server = await shelf_io.serve(handler, InternetAddress.anyIPv4, 8080);
    print('Signaling server running on ws://${_server!.address.address}:${_server!.port}');
  }

  void stop() {
    _server?.close();
    for (var client in _clients) {
      client.sink.close();
    }
    _clients.clear();
  }
  
  // Helper to get local IP to show in UI
  Future<String> getIpAddress() async {
    for (var interface in await NetworkInterface.list()) {
      for (var addr in interface.addresses) {
        if (addr.type == InternetAddressType.IPv4 && !addr.isLoopback) {
          return addr.address;
        }
      }
    }
    return 'Unknown IP';
  }
}
