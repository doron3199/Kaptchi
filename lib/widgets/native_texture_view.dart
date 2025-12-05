import 'dart:async';
import 'package:flutter/material.dart';
import '../services/native_camera_service.dart';

class NativeTextureView extends StatefulWidget {
  const NativeTextureView({super.key});

  @override
  State<NativeTextureView> createState() => _NativeTextureViewState();
}

class _NativeTextureViewState extends State<NativeTextureView> {
  double _aspectRatio = 16 / 9;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(const Duration(milliseconds: 500), (timer) {
      _checkAspectRatio();
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  void _checkAspectRatio() {
    final service = NativeCameraService();
    final width = service.getFrameWidth();
    final height = service.getFrameHeight();
    
    if (width > 0 && height > 0) {
      final newRatio = width / height;
      if ((newRatio - _aspectRatio).abs() > 0.01) {
        if (mounted) {
          setState(() {
            _aspectRatio = newRatio;
          });
        }
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final textureId = NativeCameraService().getTextureId();
    
    if (textureId == -1) {
      return const Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            CircularProgressIndicator(),
            SizedBox(height: 16),
            Text('Initializing Graphics...', style: TextStyle(color: Colors.white)),
          ],
        ),
      );
    }

    return Center(
      child: AspectRatio(
        aspectRatio: _aspectRatio,
        child: Texture(textureId: textureId),
      ),
    );
  }
}
