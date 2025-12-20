// import 'dart:io';
import 'package:flutter/material.dart';
import 'package:camera/camera.dart';
import 'package:qr_flutter/qr_flutter.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';
import '../services/native_camera_service.dart';
import '../services/gallery_service.dart';

class CameraListWidget extends StatelessWidget {
  final List<CameraDescription> cameras;
  final Function(int) onSelectCamera;
  final bool shrinkWrap;
  final ScrollPhysics? physics;

  const CameraListWidget({
    super.key,
    required this.cameras,
    required this.onSelectCamera,
    this.shrinkWrap = false,
    this.physics,
  });

  @override
  Widget build(BuildContext context) {
    if (cameras.isEmpty) {
      return Center(
        child: Text(
          AppLocalizations.of(context)!.noCamerasFound,
          style: const TextStyle(color: Colors.white),
        ),
      );
    }
    return ListView.builder(
      shrinkWrap: shrinkWrap,
      physics: physics,
      itemCount: cameras.length,
      itemBuilder: (context, index) {
        final camera = cameras[index];
        final cleanName = camera.name.replaceAll(RegExp(r'<[^>]*>'), '').trim();
        return Card(
          color: Colors.blue[900],
          margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
          child: ListTile(
            leading: const Icon(Icons.camera_alt, color: Colors.white),
            title: Text(cleanName, style: const TextStyle(color: Colors.white)),
            onTap: () => onSelectCamera(index),
          ),
        );
      },
    );
  }
}

class ScreenCaptureWidget extends StatelessWidget {
  final Function(int) onSelectScreen;
  final VoidCallback onSelectWindow;

  const ScreenCaptureWidget({
    super.key,
    required this.onSelectScreen,
    required this.onSelectWindow,
  });

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const Divider(color: Colors.white24, height: 32),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16.0),
          child: const Text(
            'Screen Capture',
            style: TextStyle(
              fontSize: 16,
              fontWeight: FontWeight.bold,
              color: Colors.white70,
            ),
          ),
        ),
        const SizedBox(height: 8),
        ..._buildMonitorList(),
        Card(
          color: Colors.teal[800],
          margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
          child: ListTile(
            leading: const Icon(Icons.window, color: Colors.white),
            title: const Text(
              'Capture Window...',
              style: TextStyle(color: Colors.white),
            ),
            onTap: onSelectWindow,
          ),
        ),
      ],
    );
  }

  List<Widget> _buildMonitorList() {
    final monitors = NativeCameraService().getMonitors();
    if (monitors.isEmpty) {
      return [
        Card(
          color: Colors.teal[800],
          margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
          child: ListTile(
            leading: const Icon(Icons.desktop_windows, color: Colors.white),
            title: const Text(
              'Capture Full Screen',
              style: TextStyle(color: Colors.white),
            ),
            onTap: () => onSelectScreen(0),
          ),
        ),
      ];
    }
    return monitors.map((monitor) {
      return Card(
        color: Colors.teal[800],
        margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
        child: ListTile(
          leading: const Icon(Icons.monitor, color: Colors.white),
          title: Text(
            monitor.name,
            style: const TextStyle(color: Colors.white),
            overflow: TextOverflow.ellipsis,
          ),
          subtitle: Text(
            '${monitor.width}x${monitor.height}',
            style: const TextStyle(color: Colors.white60, fontSize: 12),
          ),
          onTap: () => onSelectScreen(monitor.index),
        ),
      );
    }).toList();
  }
}

class ExportSectionWidget extends StatelessWidget {
  final TextEditingController pdfNameController;
  final TextEditingController pdfPathController;
  final VoidCallback onPickDirectory;
  final VoidCallback onExportPdf;

  const ExportSectionWidget({
    super.key,
    required this.pdfNameController,
    required this.pdfPathController,
    required this.onPickDirectory,
    required this.onExportPdf,
  });

  @override
  Widget build(BuildContext context) {
    if (GalleryService.instance.images.isEmpty) return const SizedBox.shrink();

    return Padding(
      padding: const EdgeInsets.all(16.0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Divider(color: Colors.white24),
          Text(
            AppLocalizations.of(context)!.exportSettings,
            style: const TextStyle(
              color: Colors.white,
              fontWeight: FontWeight.bold,
            ),
          ),
          const SizedBox(height: 8),
          TextField(
            controller: pdfNameController,
            style: const TextStyle(color: Colors.white),
            decoration: InputDecoration(
              labelText: AppLocalizations.of(context)!.fileName,
              labelStyle: const TextStyle(color: Colors.white70),
              enabledBorder: const UnderlineInputBorder(
                borderSide: BorderSide(color: Colors.white70),
              ),
            ),
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: pdfPathController,
                  style: const TextStyle(color: Colors.white, fontSize: 12),
                  readOnly: true,
                  decoration: InputDecoration(
                    labelText: AppLocalizations.of(context)!.saveLocation,
                    labelStyle: const TextStyle(color: Colors.white70),
                    enabledBorder: const UnderlineInputBorder(
                      borderSide: BorderSide(color: Colors.white70),
                    ),
                  ),
                ),
              ),
              IconButton(
                icon: const Icon(Icons.folder_open, color: Colors.blue),
                onPressed: onPickDirectory,
              ),
            ],
          ),
          const SizedBox(height: 16),
          SizedBox(
            width: double.infinity,
            child: ElevatedButton.icon(
              onPressed: onExportPdf,
              icon: const Icon(Icons.picture_as_pdf),
              label: Text(
                AppLocalizations.of(
                  context,
                )!.exportPdf(GalleryService.instance.images.length),
              ),
              style: ElevatedButton.styleFrom(
                backgroundColor: Colors.indigo,
                foregroundColor: Colors.white,
                padding: const EdgeInsets.symmetric(vertical: 16),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class MobileConnectionSection extends StatelessWidget {
  final List<({String name, String ip})> availableInterfaces;
  final String serverIp;
  final bool isMediaServerRunning;
  final Function(String?) onSelectIp;

  const MobileConnectionSection({
    super.key,
    required this.availableInterfaces,
    required this.serverIp,
    required this.isMediaServerRunning,
    required this.onSelectIp,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      color: Colors.black,
      child: SingleChildScrollView(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const SizedBox(height: 32),
            Text(
              AppLocalizations.of(context)!.connectMobileCamera,
              style: const TextStyle(
                fontSize: 24,
                fontWeight: FontWeight.bold,
                color: Colors.white,
              ),
            ),
            const SizedBox(height: 16),
            if (availableInterfaces.isNotEmpty)
              Padding(
                padding: const EdgeInsets.symmetric(
                  horizontal: 64.0,
                  vertical: 8.0,
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      AppLocalizations.of(context)!.serverInterface,
                      style: const TextStyle(color: Colors.grey, fontSize: 12),
                    ),
                    DropdownButton<String>(
                      value: serverIp.isNotEmpty ? serverIp : null,
                      isExpanded: true,
                      dropdownColor: Colors.grey[900],
                      style: const TextStyle(color: Colors.white, fontSize: 16),
                      iconEnabledColor: Colors.white,
                      underline: Container(height: 1, color: Colors.white54),
                      items: availableInterfaces.map((i) {
                        return DropdownMenuItem(
                          value: i.ip,
                          child: Text('${i.name} (${i.ip})'),
                        );
                      }).toList(),
                      onChanged: onSelectIp,
                    ),
                  ],
                ),
              ),
            if (serverIp.isNotEmpty) ...[
              Container(
                color: Colors.white,
                padding: const EdgeInsets.all(16),
                child: Semantics(
                  label: 'QR Code for server: rtmp://${serverIp}/live/stream',
                  child: QrImageView(
                    data: 'rtmp://$serverIp/live/stream',
                    version: QrVersions.auto,
                    size: 250.0,
                  ),
                ),
              ),
              const SizedBox(height: 16),
              Text(
                AppLocalizations.of(context)!.scanWithApp,
                style: TextStyle(fontSize: 16, color: Colors.grey[400]),
              ),
              const SizedBox(height: 16),
              if (isMediaServerRunning)
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
                      style: const TextStyle(color: Colors.green, fontSize: 12),
                    ),
                  ],
                )
              else
                Text(
                  AppLocalizations.of(context)!.mediaServerStopped,
                  style: const TextStyle(color: Colors.grey, fontSize: 12),
                  textAlign: TextAlign.center,
                ),
            ],
          ],
        ),
      ),
    );
  }
}
