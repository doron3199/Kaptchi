import 'dart:async';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;

class MediaServerService {
  static final MediaServerService instance = MediaServerService._();
  
  MediaServerService._();

  Process? _process;
  bool get isRunning => _process != null;

  // Stream controller to notify when a publisher connects
  final _streamStartedController = StreamController<String>.broadcast();
  Stream<String> get onStreamStarted => _streamStartedController.stream;

  Future<bool> startServer({String? executablePath}) async {
    if (_process != null) return true;

    try {
      // 1. Locate the executable
      String exePath;
      
      if (executablePath != null && File(executablePath).existsSync()) {
        exePath = executablePath;
      } else {
        // Default logic
        if (kDebugMode) {
          // Assuming running from VS Code, Directory.current is project root
          exePath = p.join(Directory.current.path, 'server', 'bin', 'mediamtx.exe');
        } else {
          // In release, look in the same directory as the app executable
          final exeDir = p.dirname(Platform.resolvedExecutable);
          exePath = p.join(exeDir, 'mediamtx.exe');
        }

        if (!File(exePath).existsSync()) {
          debugPrint('Media Server executable not found at: $exePath');
          // Fallback: Try looking in a 'server' subfolder next to executable
          if (!kDebugMode) {
             final exeDir = p.dirname(Platform.resolvedExecutable);
             exePath = p.join(exeDir, 'server', 'mediamtx.exe');
             if (!File(exePath).existsSync()) {
               return false;
             }
          } else {
             return false;
          }
        }
      }

      debugPrint('Starting Media Server from: $exePath');

      // 2. Start the process
      // Use normal mode to capture output for debugging
      // Pass the config file explicitly
      String configPath = p.join(Directory.current.path, 'mediamtx.yml');
      if (!File(configPath).existsSync()) {
         // Try to find it in the same dir as the exe
         configPath = p.join(p.dirname(exePath), 'mediamtx.yml');
      }

      debugPrint('Using config file: $configPath');

      // Check if mediamtx is already running and kill it
      if (Platform.isWindows) {
        try {
          final result = await Process.run('tasklist', ['/FI', 'IMAGENAME eq mediamtx.exe']);
          if (result.stdout.toString().contains('mediamtx.exe')) {
            debugPrint('Found existing mediamtx.exe, killing it...');
            await Process.run('taskkill', ['/F', '/IM', 'mediamtx.exe']);
            // Give it a moment to release resources
            await Future.delayed(const Duration(milliseconds: 500));
          }
        } catch (e) {
          debugPrint('Error checking/killing existing process: $e');
        }
      }

      _process = await Process.start(
        exePath,
        [configPath], // Pass config file as argument
        mode: ProcessStartMode.normal, 
      );

      // Pipe output to debug console
      _process!.stdout.listen((data) {
        final message = String.fromCharCodes(data).trim();
        if (message.isNotEmpty) {
          debugPrint('[MediaMTX] $message');
          // Detect publishing event
          // Example log: [RTMP] [conn 192.168.1.10:55555] is publishing to path 'live/stream'
          // Or: [path live/stream] [RTMP source] ready
          if (message.contains('is publishing to path') || message.contains('ready')) {
             // Extract path if needed, or just notify
             // Assuming standard path 'live/stream'
             // We debounce this slightly to avoid multiple triggers
             _streamStartedController.add('live/stream');
          }
        }
      });
      
      _process!.stderr.listen((data) {
        final message = String.fromCharCodes(data).trim();
        if (message.isNotEmpty) debugPrint('[MediaMTX Error] $message');
      });

      debugPrint('Media Server started with PID: ${_process!.pid}');
      return true;

    } catch (e) {
      debugPrint('Failed to start Media Server: $e');
      return false;
    }
  }

  void stopServer() {
    if (_process != null) {
      debugPrint('Stopping Media Server (PID: ${_process!.pid})...');
      _process!.kill();
      _process = null;
    }
  }
}
