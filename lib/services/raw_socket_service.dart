import 'dart:io';
import 'dart:convert';
import 'dart:async';
import 'package:flutter/foundation.dart';

class RawSocketService {
  static final RawSocketService instance = RawSocketService._internal();
  
  factory RawSocketService() {
    return instance;
  }
  
  RawSocketService._internal();

  ServerSocket? _server;
  Socket? _clientSocket; // For Android client
  final List<Socket> _connectedClients = []; // For Windows server
  
  // Callbacks
  Function(String command, Map<String, dynamic> data)? onControlMessage;

  // Windows: Start Server
  Future<void> startServer() async {
    if (_server != null) return;

    try {
      _server = await ServerSocket.bind(InternetAddress.anyIPv4, 4040);
      debugPrint("RawSocketService: Listening on port 4040...");

      _server!.listen((Socket client) {
        debugPrint('RawSocketService: Connection from: ${client.remoteAddress.address}:${client.remotePort}');
        _connectedClients.add(client);

        client.listen(
          (List<int> data) {
            String message = utf8.decode(data);
            // debugPrint("RawSocketService (Server) Received: $message");
            _handleMessage(message);
          },
          onDone: () {
            debugPrint('RawSocketService: Client disconnected');
            _connectedClients.remove(client);
            client.destroy();
          },
          onError: (e) {
            debugPrint('RawSocketService: Client error: $e');
            _connectedClients.remove(client);
            client.destroy();
          },
        );
      });
    } catch (e) {
      debugPrint('RawSocketService: Error starting server: $e');
    }
  }

  // Android: Connect to Server
  Future<void> connect(String ip) async {
    if (_clientSocket != null) {
        _clientSocket!.destroy();
        _clientSocket = null;
    }

    try {
      debugPrint("RawSocketService: Connecting to $ip:4040...");
      _clientSocket = await Socket.connect(ip, 4040);
      debugPrint("RawSocketService: Connected");

      _clientSocket!.listen(
        (List<int> data) {
          String message = utf8.decode(data);
          // debugPrint("RawSocketService (Client) Received: $message");
          _handleMessage(message);
        },
        onDone: () {
          debugPrint("RawSocketService: Connection closed");
          _clientSocket = null;
        },
        onError: (e) {
          debugPrint("RawSocketService: Connection error: $e");
          _clientSocket = null;
        },
      );
    } catch (e) {
      debugPrint("RawSocketService: Connection failed: $e");
    }
  }

  // Send message (from Server to all clients, or Client to Server)
  void send(String type, Map<String, dynamic> payload) {
    final msg = jsonEncode({
      'type': type,
      'payload': payload,
    });

    if (_clientSocket != null) {
      // Client sending to server
      try {
        _clientSocket!.write(msg);
      } catch (e) {
        debugPrint("RawSocketService: Error sending data: $e");
      }
    } else {
      // Server broadcasting to clients
      for (var client in _connectedClients) {
        try {
          client.write(msg);
        } catch (e) {
          debugPrint("RawSocketService: Error broadcasting to client: $e");
        }
      }
    }
  }

  void _handleMessage(String message) {
    try {
      // Handle potential multiple JSON objects in one packet
      // This is a naive implementation, assuming clean JSONs or single messages
      // For better robustness, we should split by some delimiter or try to parse iteratively
      
      // Simple attempt to handle concatenated JSONs like "{...}{...}"
      int braceCount = 0;
      int startIndex = 0;
      
      for (int i = 0; i < message.length; i++) {
        if (message[i] == '{') {
          braceCount++;
        } else if (message[i] == '}') {
          braceCount--;
          if (braceCount == 0) {
            String jsonStr = message.substring(startIndex, i + 1);
            _processSingleJson(jsonStr);
            startIndex = i + 1;
          }
        }
      }
    } catch (e) {
      debugPrint("RawSocketService: Error parsing message: $e");
    }
  }

  void _processSingleJson(String jsonStr) {
      try {
        Map<String, dynamic> data = jsonDecode(jsonStr);
        String? type = data['type'];
        var payload = data['payload'];

        if (type == 'control') {
          if (payload != null) {
              String? command = payload['command'];
              var data = payload['data'];
              if (command != null && data != null) {
                onControlMessage?.call(command, data);
              }
          }
        }
      } catch (e) {
        debugPrint("RawSocketService: Error processing JSON: $e");
      }
  }
  
  void stop() {
      _server?.close();
      _server = null;
      _clientSocket?.destroy();
      _clientSocket = null;
      for(var c in _connectedClients) {
          c.destroy();
      }
      _connectedClients.clear();
  }

    // Helper to get local IP to show in UI
  Future<List<({String name, String ip})>> getNetworkInterfaces() async {
    final List<({String name, String ip})> interfacesList = [];
    try {
      debugPrint('DEBUG: Listing network interfaces...');
      final interfaces = await NetworkInterface.list(
        includeLoopback: true, 
        includeLinkLocal: true,
        type: InternetAddressType.IPv4
      );

      for (var interface in interfaces) {
        debugPrint('DEBUG: Found interface: ${interface.name}');
        for (var addr in interface.addresses) {
          debugPrint('DEBUG:   Address: ${addr.address}');
          if (addr.type == InternetAddressType.IPv4) {
            // Filter out link-local (APIPA) addresses which are usually useless for this
            if (!addr.address.startsWith('169.254')) {
                interfacesList.add((name: interface.name, ip: addr.address));
            } else {
                debugPrint('DEBUG:   Skipping link-local address: ${addr.address}');
            }
          }
        }
      }
    } catch (e) {
      debugPrint('Error getting IPs: $e');
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
