import 'package:flutter/material.dart';
import '../services/native_camera_service.dart';

class NativeCameraView extends StatefulWidget {
  final double? overrideAspectRatio;
  final TransformationController? transformationController;

  const NativeCameraView({
    super.key,
    this.overrideAspectRatio,
    this.transformationController,
  });

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
        // Only start the camera if screen capture is NOT active.
        // The native Start() method stops screen capture, so we must avoid
        // calling it when screen capture is the intended source.
        if (!_service.isScreenCaptureActive()) {
          _service.start();
        }
      }
    } else {
      debugPrint("Failed to get texture ID");
    }
  }

  @override
  void dispose() {
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_textureId == null) {
      return const Center(child: CircularProgressIndicator());
    }

    final ar = widget.overrideAspectRatio;
    if (ar != null) {
      // Canvas mode: height-based sizing with pan & zoom via InteractiveViewer
      return LayoutBuilder(
        builder: (context, constraints) {
          final h = constraints.maxHeight;
          final w = h * ar;
          return InteractiveViewer(
            transformationController: widget.transformationController,
            constrained: false,
            minScale: 0.5,
            maxScale: 10.0,
            child: SizedBox(
              width: w,
              height: h,
              child: Texture(textureId: _textureId!),
            ),
          );
        },
      );
    }
    return AspectRatio(
      aspectRatio: 16 / 9,
      child: Texture(textureId: _textureId!),
    );
  }
}
