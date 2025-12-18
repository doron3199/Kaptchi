import 'package:flutter/material.dart';
import 'package:camera/camera.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

class VideoSourceSheet extends StatelessWidget {
  final List<CameraDescription> cameras;
  final Function(int) onSelectCamera;
  final VoidCallback onSelectMobile;

  const VideoSourceSheet({
    super.key,
    required this.cameras,
    required this.onSelectCamera,
    required this.onSelectMobile,
  });

  @override
  Widget build(BuildContext context) {
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
}
