import 'dart:io';
import 'package:flutter/material.dart';
import 'camera_screen.dart';
import 'monitor_screen.dart';

class HomeScreen extends StatelessWidget {
  const HomeScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Kaptchi')),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            ElevatedButton.icon(
              icon: const Icon(Icons.camera_alt),
              label: const Text('Start Camera Mode'),
              onPressed: () {
                Navigator.push(
                  context,
                  MaterialPageRoute(builder: (_) => const CameraScreen()),
                );
              },
            ),
            const SizedBox(height: 20),
            if (Platform.isWindows || Platform.isLinux || Platform.isMacOS)
              ElevatedButton.icon(
                icon: const Icon(Icons.monitor),
                label: const Text('Start Monitor Mode'),
                onPressed: () {
                  Navigator.push(
                    context,
                    MaterialPageRoute(builder: (_) => const MonitorScreen()),
                  );
                },
              ),
          ],
        ),
      ),
    );
  }
}
