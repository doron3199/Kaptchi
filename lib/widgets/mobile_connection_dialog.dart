import 'dart:async';
import 'package:flutter/material.dart';
import 'package:qr_flutter/qr_flutter.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';
import '../services/media_server_service.dart';
import '../services/raw_socket_service.dart';

class MobileConnectionDialog extends StatefulWidget {
  final VoidCallback onConnect;

  const MobileConnectionDialog({super.key, required this.onConnect});

  @override
  State<MobileConnectionDialog> createState() => _MobileConnectionDialogState();
}

class _MobileConnectionDialogState extends State<MobileConnectionDialog> {
  String _serverIp = '';
  List<({String name, String ip})> _availableInterfaces = [];
  StreamSubscription? _streamSubscription;

  @override
  void initState() {
    super.initState();
    _loadInterfaces();
    _listenForStream();
  }

  @override
  void dispose() {
    _streamSubscription?.cancel();
    super.dispose();
  }

  Future<void> _loadInterfaces() async {
    try {
      final interfaces = await RawSocketService.instance.getNetworkInterfaces();
      if (!mounted) return;
      if (interfaces.isNotEmpty) {
        setState(() {
          _availableInterfaces = interfaces;
          _serverIp = interfaces[0].ip;
        });
      }
    } catch (e) {
      debugPrint('Error getting interfaces: $e');
    }
  }

  void _listenForStream() {
    // Check if stream is already active
    if (MediaServerService.instance.isStreamActive('live/stream')) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _handleConnect();
      });
      return;
    }

    _streamSubscription = MediaServerService.instance.onStreamStarted.listen((
      path,
    ) {
      if (path == 'live/stream' && mounted) {
        _handleConnect();
      }
    });
  }

  void _handleConnect() {
    // Determine if we should pop or just call callback.
    // Usually dialogs are popped by the caller, but here we can pop ourselves.
    // However, if we pop, we must ensure we don't pop if already disposed.
    widget.onConnect();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(AppLocalizations.of(context)!.connectMobileCamera),
      content: SizedBox(
        width: 300,
        height: 450,
        child: SingleChildScrollView(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              // Server IP Selection
              if (_availableInterfaces.isNotEmpty)
                Padding(
                  padding: const EdgeInsets.only(bottom: 16.0),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        AppLocalizations.of(context)!.serverInterface,
                        style: const TextStyle(
                          color: Colors.grey,
                          fontSize: 12,
                        ),
                      ),
                      DropdownButton<String>(
                        value: _serverIp,
                        isExpanded: true,
                        items: _availableInterfaces.map((i) {
                          return DropdownMenuItem(
                            value: i.ip,
                            child: Text('${i.name} (${i.ip})'),
                          );
                        }).toList(),
                        onChanged: (val) {
                          if (val != null) {
                            setState(() {
                              _serverIp = val;
                            });
                          }
                        },
                      ),
                    ],
                  ),
                ),

              // QR Code
              if (_serverIp.isNotEmpty) ...[
                Container(
                  color: Colors.white,
                  padding: const EdgeInsets.all(16),
                  child: QrImageView(
                    data: 'rtmp://$_serverIp/live/stream',
                    version: QrVersions.auto,
                    size: 200.0,
                  ),
                ),
                const SizedBox(height: 16),
                Text(
                  AppLocalizations.of(context)!.scanWithApp,
                  style: TextStyle(fontSize: 16, color: Colors.grey[600]),
                  textAlign: TextAlign.center,
                ),
                const SizedBox(height: 16),

                // Media Server Status
                if (MediaServerService.instance.isRunning)
                  Row(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Icon(
                        Icons.check_circle,
                        color: Colors.green,
                        size: 16,
                      ),
                      const SizedBox(width: 8),
                      Text(
                        AppLocalizations.of(context)!.mediaServerRunning,
                        style: const TextStyle(
                          color: Colors.green,
                          fontSize: 12,
                        ),
                      ),
                    ],
                  )
                else
                  Text(
                    AppLocalizations.of(context)!.mediaServerStopped,
                    style: const TextStyle(color: Colors.red, fontSize: 12),
                    textAlign: TextAlign.center,
                  ),
              ],
            ],
          ),
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: Text(AppLocalizations.of(context)!.close),
        ),
      ],
    );
  }
}
