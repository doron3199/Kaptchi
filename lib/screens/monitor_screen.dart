import 'package:flutter/material.dart';
import '../services/native_camera_service.dart';
import '../widgets/native_texture_view.dart';

class MonitorScreen extends StatefulWidget {
  const MonitorScreen({super.key});

  @override
  State<MonitorScreen> createState() => _MonitorScreenState();
}

class _MonitorScreenState extends State<MonitorScreen> {
  // Default to the RTMP port configured in mediamtx.yml
  final TextEditingController _urlController = TextEditingController(text: 'rtmp://localhost/live/stream');
  bool _isPlaying = false;

  @override
  void initState() {
    super.initState();
    // Auto-play on start
    _playStream();
  }

  void _playStream() {
    if (_urlController.text.isNotEmpty) {
      NativeCameraService().startStream(_urlController.text);
      setState(() {
        _isPlaying = true;
      });
    }
  }

  @override
  void dispose() {
    NativeCameraService().stop();
    _urlController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Monitor Mode'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: () {
              _playStream();
            },
          ),
        ],
      ),
      body: Column(
        children: [
          // URL Input for debugging
          Padding(
            padding: const EdgeInsets.all(8.0),
            child: Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _urlController,
                    decoration: const InputDecoration(
                      labelText: 'Stream URL',
                      border: OutlineInputBorder(),
                      hintText: 'rtmp://localhost/live/stream',
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                ElevatedButton(
                  onPressed: _playStream,
                  child: const Text('Play'),
                ),
              ],
            ),
          ),
          
          // Video Player
          Expanded(
            child: Container(
              color: Colors.black,
              child: Center(
                child: _isPlaying 
                  ? const NativeTextureView()
                  : const Text('Enter URL and press Play', style: TextStyle(color: Colors.white)),
              ),
            ),
          ),
        ],
      ),
    );
  }
}
