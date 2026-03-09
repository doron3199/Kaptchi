import 'dart:io';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import '../services/native_camera_service.dart';

/// Wraps the camera view with gesture-based pan/zoom for the whiteboard canvas.
///
/// When [isCanvasViewMode] is true, drag and pinch gestures update the canvas
/// viewport via [SetPanoramaViewport] / [SetCanvasViewMode].  When false, the
/// widget is transparent and gestures fall through to the underlying
/// [ZoomableStreamView] / [NativeCameraView].
///
/// Phase 2: when [canvasTextureId] >= 0, a dedicated [Texture] widget is shown
/// for the canvas instead of relying on the main camera texture.
class CanvasGestureView extends StatefulWidget {
  final bool isCanvasViewMode;
  final int canvasTextureId;
  final double initialPanX;
  final double initialPanY;
  final double initialZoom;
  final ValueChanged<({double panX, double panY, double zoom})>? onViewportChanged;
  final Widget child;

  const CanvasGestureView({
    super.key,
    required this.isCanvasViewMode,
    required this.canvasTextureId,
    required this.child,
    this.initialPanX = 0.5,
    this.initialPanY = 0.5,
    this.initialZoom = 1.0,
    this.onViewportChanged,
  });

  @override
  State<CanvasGestureView> createState() => _CanvasGestureViewState();
}

class _CanvasGestureViewState extends State<CanvasGestureView> {
  late double _panX;
  late double _panY;
  late double _zoom;

  // Gesture tracking
  Offset? _lastFocalPoint;
  double? _lastScaleDistance;

  @override
  void initState() {
    super.initState();
    _panX = widget.initialPanX;
    _panY = widget.initialPanY;
    _zoom = widget.initialZoom;
  }

  @override
  void didUpdateWidget(covariant CanvasGestureView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.initialPanX != widget.initialPanX ||
        oldWidget.initialPanY != widget.initialPanY ||
        oldWidget.initialZoom != widget.initialZoom) {
      _panX = widget.initialPanX;
      _panY = widget.initialPanY;
      _zoom = widget.initialZoom;
    }
  }

  void _updateViewport() {
    if (!Platform.isWindows) return;
    NativeCameraService().setPanoramaViewport(_panX, _panY, _zoom);
    widget.onViewportChanged?.call((panX: _panX, panY: _panY, zoom: _zoom));
  }

  void _handlePointerSignal(PointerSignalEvent event) {
    if (event is! PointerScrollEvent) return;

    const double scrollPanSensitivity = 0.0015;
    const double zoomInFactor = 1.1;
    const double zoomOutFactor = 0.9;

    if (event.scrollDelta.dx.abs() > event.scrollDelta.dy.abs() &&
        event.scrollDelta.dx.abs() > 0.0) {
      _panX += event.scrollDelta.dx * scrollPanSensitivity / _zoom;
      _panX = _panX.clamp(0.0, 1.0);
    } else if (event.scrollDelta.dy.abs() > 0.0) {
      _zoom *= event.scrollDelta.dy > 0 ? zoomOutFactor : zoomInFactor;
      _zoom = _zoom.clamp(1.0, 8.0);
    }

    _updateViewport();
  }

  void _applyPanDelta(Offset delta) {
    const double panSensitivity = 0.002;
    _panX -= delta.dx * panSensitivity / _zoom;
    _panY -= delta.dy * panSensitivity / _zoom;
    _panX = _panX.clamp(0.0, 1.0);
    _panY = _panY.clamp(0.0, 1.0);
    _updateViewport();
  }

  @override
  Widget build(BuildContext context) {
    // Phase 2: use dedicated canvas texture
    final bool useCanvasTexture =
        widget.isCanvasViewMode && widget.canvasTextureId >= 0;

    Widget content;
    if (useCanvasTexture) {
      content = Texture(textureId: widget.canvasTextureId);
    } else {
      content = widget.child;
    }

    if (!widget.isCanvasViewMode) {
      // Not in canvas view — pass gestures through unchanged
      return content;
    }

    // Canvas view mode: intercept gestures for pan/zoom
    return Listener(
      onPointerSignal: _handlePointerSignal,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onPanUpdate: (details) {
          _applyPanDelta(details.delta);
        },
        onScaleStart: (details) {
          _lastFocalPoint = details.focalPoint;
          _lastScaleDistance = null;
        },
        onScaleUpdate: (details) {
          final size = context.size;
          if (size == null || _lastFocalPoint == null) return;

          // Pan: translate focal-point delta into normalized canvas movement
          final delta = details.focalPoint - _lastFocalPoint!;
          _lastFocalPoint = details.focalPoint;

          _applyPanDelta(delta);

          // Zoom: pinch scale
          if (details.pointerCount >= 2) {
            final newScale = details.scale;
            if (_lastScaleDistance != null) {
              _zoom *= newScale / _lastScaleDistance!;
              _zoom = _zoom.clamp(1.0, 8.0);
            }
            _lastScaleDistance = newScale;
            _updateViewport();
          }
        },
        onScaleEnd: (_) {
          _lastFocalPoint = null;
          _lastScaleDistance = null;
        },
        child: content,
      ),
    );
  }
}
