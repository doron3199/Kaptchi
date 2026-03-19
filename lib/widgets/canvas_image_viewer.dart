import 'dart:async';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import '../services/native_camera_service.dart';

/// Displays the full-resolution whiteboard canvas as a cached [ui.Image]
/// inside an [InteractiveViewer] for instant pan/zoom with zero C++ round-trips.
///
/// Polls [NativeCameraService.getCanvasVersion] every 500 ms and re-fetches
/// the RGBA buffer only when the version changes.
class CanvasImageViewer extends StatefulWidget {
  const CanvasImageViewer({super.key});

  @override
  State<CanvasImageViewer> createState() => _CanvasImageViewerState();
}

class _CanvasImageViewerState extends State<CanvasImageViewer> {
  ui.Image? _image;
  Timer? _pollTimer;
  bool _fetching = false;

  @override
  void initState() {
    super.initState();
    _fetchImage();
    _pollTimer = Timer.periodic(const Duration(milliseconds: 500), (_) {
      if (!mounted) return;
      _fetchImage();
    });
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _image?.dispose();
    super.dispose();
  }

  Future<void> _fetchImage() async {
    if (_fetching) return;
    _fetching = true;

    try {
      final result = NativeCameraService().getCanvasFullResRgba();
      if (result == null || !mounted) return;

      final completer = Completer<ui.Image>();
      ui.decodeImageFromPixels(
        result.bytes,
        result.width,
        result.height,
        ui.PixelFormat.rgba8888,
        (image) => completer.complete(image),
      );

      final newImage = await completer.future;
      if (!mounted) {
        newImage.dispose();
        return;
      }

      final oldImage = _image;
      setState(() {
        _image = newImage;
      });
      oldImage?.dispose();
    } finally {
      _fetching = false;
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_image == null) {
      return Container(
        color: Colors.white,
        child: const Center(
          child: CircularProgressIndicator(),
        ),
      );
    }

    return Container(
      color: Colors.white,
      child: InteractiveViewer(
        minScale: 0.5,
        maxScale: 10.0,
        child: Center(
          child: RawImage(
            image: _image,
            fit: BoxFit.contain,
            width: double.infinity,
            height: double.infinity,
          ),
        ),
      ),
    );
  }
}
