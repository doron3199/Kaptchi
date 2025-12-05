import 'package:flutter/material.dart';
import '../services/native_camera_service.dart';

class NativeCameraView extends StatefulWidget {
  const NativeCameraView({super.key});

  @override
  State<NativeCameraView> createState() => _NativeCameraViewState();
}

class _NativeCameraViewState extends State<NativeCameraView> {
  final NativeCameraService _service = NativeCameraService();
  int? _textureId;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      // Yield to event loop to ensure scheduler is idle before calling native code
      // which might pump the message loop and trigger re-entrant frames.
      await Future.delayed(Duration.zero);
      if (mounted) {
        _initializeCamera();
      }
    });
  }

  Future<void> _initializeCamera() async {
    // Ensure service is initialized
    _service.initialize();
    
    final textureId = _service.getTextureId();
    if (textureId != -1) {
      if (mounted) {
        setState(() {
          _textureId = textureId;
        });
        _service.start();
      }
    } else {
      debugPrint("Failed to get texture ID");
    }
  }

  @override
  void dispose() {
    _service.stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_textureId == null) {
      return const Center(child: CircularProgressIndicator());
    }
    
    return AspectRatio(
      aspectRatio: 16 / 9,
      child: Texture(textureId: _textureId!),
    );
  }
}
