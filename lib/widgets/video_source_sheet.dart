import 'dart:io';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';
import '../services/native_camera_service.dart';

class VideoSourceSheet extends StatelessWidget {
  final List<CameraDescription> cameras;
  final Function(int) onSelectCamera;
  final VoidCallback onSelectMobile;

  /// Callback for screen capture: (monitorIndex, windowHandle)
  /// windowHandle = 0 means capture full monitor
  final Function(int monitorIndex, int windowHandle)? onSelectScreenCapture;

  const VideoSourceSheet({
    super.key,
    required this.cameras,
    required this.onSelectCamera,
    required this.onSelectMobile,
    this.onSelectScreenCapture,
  });

  @override
  Widget build(BuildContext context) {
    // Get available monitors
    final monitors = Platform.isWindows && onSelectScreenCapture != null
        ? NativeCameraService().getMonitors()
        : <MonitorInfo>[];

    return Container(
      padding: const EdgeInsets.all(16),
      child: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              AppLocalizations.of(context)!.selectVideoSource,
              style: const TextStyle(fontSize: 20, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 16),

            // Local Cameras
            if (cameras.isNotEmpty) ...[
              Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  AppLocalizations.of(context)!.localCameras,
                  style: const TextStyle(
                    fontWeight: FontWeight.bold,
                    color: Colors.grey,
                  ),
                ),
              ),
              ...cameras.asMap().entries.map((entry) {
                final index = entry.key;
                final camera = entry.value;
                // Clean up camera name by removing ID part in angle brackets <...>
                final cleanName = camera.name
                    .replaceAll(RegExp(r'<[^>]*>'), '')
                    .trim();

                return ListTile(
                  leading: const Icon(Icons.camera),
                  title: Text(cleanName),
                  onTap: () {
                    // Fix for swapped cameras when exactly 2 are present
                    // The user reported that selecting one opens the other.
                    int targetIndex = index;
                    if (cameras.length == 2) {
                      targetIndex = (index == 0) ? 1 : 0;
                    }
                    onSelectCamera(targetIndex);
                  },
                );
              }),
              const Divider(),
            ],

            // Screen Capture (Windows only)
            if (Platform.isWindows && onSelectScreenCapture != null) ...[
              Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  AppLocalizations.of(context)!.screenCapture,
                  style: const TextStyle(
                    fontWeight: FontWeight.bold,
                    color: Colors.grey,
                  ),
                ),
              ),
              // List all monitors
              ...monitors.map(
                (monitor) => ListTile(
                  leading: const Icon(Icons.desktop_windows),
                  title: Text(
                    AppLocalizations.of(context)!.captureMonitor(monitor.name),
                  ),
                  onTap: () {
                    Navigator.pop(context); // Close sheet first
                    onSelectScreenCapture!(monitor.index, 0); // 0 = full screen
                  },
                ),
              ),
              ListTile(
                leading: const Icon(Icons.window),
                title: Text(AppLocalizations.of(context)!.captureWindow),
                onTap: () {
                  _showWindowPicker(context);
                },
              ),
              const Divider(),
            ],

            Align(
              alignment: Alignment.centerLeft,
              child: Text(
                AppLocalizations.of(context)!.remoteStreams,
                style: const TextStyle(
                  fontWeight: FontWeight.bold,
                  color: Colors.grey,
                ),
              ),
            ),
            ListTile(
              leading: const Icon(Icons.phone_android),
              title: Text(AppLocalizations.of(context)!.mobileApp),
              subtitle: Text(AppLocalizations.of(context)!.connectViaQr),
              onTap: onSelectMobile,
            ),
          ],
        ),
      ),
    );
  }

  void _showWindowPicker(BuildContext context) {
    final cameraService = NativeCameraService();
    final windows = cameraService.getCapturableWindows();

    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(AppLocalizations.of(context)!.selectWindowToCapture),
        content: SizedBox(
          width: 400,
          height: 400,
          child: ListView.builder(
            itemCount: windows.length,
            itemBuilder: (context, index) {
              final window = windows[index];
              return ListTile(
                leading: const Icon(Icons.window),
                title: Text(window.title, overflow: TextOverflow.ellipsis),
                onTap: () {
                  Navigator.of(context).pop();
                  Navigator.of(context).pop(); // Close sheet too
                  // Window captures from primary monitor (0), with specific window handle
                  onSelectScreenCapture!(0, window.handle);
                },
              );
            },
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(),
            child: Text(AppLocalizations.of(context)!.cancel),
          ),
        ],
      ),
    );
  }
}
