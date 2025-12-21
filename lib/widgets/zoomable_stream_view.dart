import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

/// A widget that provides zoom and pan functionality for a stream view.
///
/// Uses Flutter's [InteractiveViewer] with a [TransformationController] for
/// smooth, reliable gesture handling.
class ZoomableStreamView extends StatefulWidget {
  final Widget child;
  final double currentZoom;
  final Offset viewOffset;
  final bool isDigitalZoomOverride;
  final double lockedPhoneZoom;
  final bool isStreamMode;
  final double phoneMaxZoom;
  final Function(double zoom, Offset offset, Size viewportSize)
  onTransformChanged;
  final Function(double) onSendZoomCommand;

  const ZoomableStreamView({
    super.key,
    required this.child,
    required this.currentZoom,
    required this.viewOffset,
    required this.isDigitalZoomOverride,
    required this.lockedPhoneZoom,
    required this.isStreamMode,
    required this.phoneMaxZoom,
    required this.onTransformChanged,
    required this.onSendZoomCommand,
  });

  @override
  State<ZoomableStreamView> createState() => _ZoomableStreamViewState();
}

class _ZoomableStreamViewState extends State<ZoomableStreamView> {
  TransformationController?
  _controller; // Kept only if needed for safe dispose, but we will remove usage.
  Size _viewportSize = Size.zero;

  // State for manual gestures
  Offset _panOffset = Offset.zero;
  double _baseScale = 1.0;
  Offset _basePanOffset = Offset.zero;
  Offset _startFocalPoint = Offset.zero;

  // Local zoom tracking for immediate response (avoids event loop lag)
  double _localZoom = 1.0;

  /// The minimum total zoom level allowed
  double get _minTotalZoom =>
      widget.isDigitalZoomOverride ? widget.lockedPhoneZoom : 1.0;

  /// Maximum total zoom level (phone zoom * digital zoom)
  static const double _maxTotalZoom = 100.0;

  @override
  void initState() {
    super.initState();
    _localZoom = widget.currentZoom;
  }

  @override
  void didUpdateWidget(ZoomableStreamView oldWidget) {
    super.didUpdateWidget(oldWidget);
    // Sync local zoom if parent updates it (e.g. from reset or other logic)
    if (oldWidget.currentZoom != widget.currentZoom) {
      _localZoom = widget.currentZoom;
    }
  }

  @override
  void dispose() {
    _controller?.dispose();
    super.dispose();
  }

  void _onScaleStart(ScaleStartDetails details) {
    _baseScale = _localZoom;
    _basePanOffset = _panOffset;
    _startFocalPoint = details.localFocalPoint;
  }

  void _onScaleUpdate(ScaleUpdateDetails details) {
    // 1. Calculate new Total Zoom
    double newTotalZoom = (_baseScale * details.scale).clamp(
      _minTotalZoom,
      _maxTotalZoom,
    );

    // 2. Determine Optical vs Digital limits
    double limit = widget.phoneMaxZoom;
    if (widget.isDigitalZoomOverride) {
      limit = widget.lockedPhoneZoom;
    }

    double opticalZoom = newTotalZoom.clamp(1.0, limit);

    // 3. Calculate Visual Scales for Focal Point Logic
    double baseVisualScale = 1.0;
    if (_baseScale > limit) {
      baseVisualScale = _baseScale / limit;
    }

    double newVisualScale = 1.0;
    if (newTotalZoom > limit) {
      newVisualScale = newTotalZoom / limit;
    }

    // 4. Calculate Pan (Focal Point Zooming)
    Offset newPanOffset = Offset.zero;

    if (newVisualScale > 1.0) {
      // Calculate the point in the content (relative to origin) that was under the focal point at start
      final contentPoint =
          (_startFocalPoint - _basePanOffset) / baseVisualScale;

      // Adjust translation so that contentPoint is now under the NEW focal point
      // T_new = P_new - (C * S_new)
      newPanOffset = details.localFocalPoint - (contentPoint * newVisualScale);
    }

    // 5. Clamp Pan Offset
    // With Alignment.topLeft, translation (0,0) is top-left aligned.
    // As scale increases, content grows right/down.
    // To keep content filling the viewport, translation must be negative or zero.
    // Valid Range: [Viewport - ScaledContent, 0]
    if (_viewportSize != Size.zero && newVisualScale > 1.0) {
      final scaledW = _viewportSize.width * newVisualScale;
      final scaledH = _viewportSize.height * newVisualScale;

      final minX = _viewportSize.width - scaledW;
      final minY = _viewportSize.height - scaledH;

      newPanOffset = Offset(
        newPanOffset.dx.clamp(minX, 0.0),
        newPanOffset.dy.clamp(minY, 0.0),
      );
    } else {
      newPanOffset = Offset.zero;
    }

    setState(() {
      _panOffset = newPanOffset;
      _localZoom = newTotalZoom;
    });

    widget.onTransformChanged(newTotalZoom, _panOffset, _viewportSize);

    if (widget.isDigitalZoomOverride) {
      widget.onSendZoomCommand(widget.lockedPhoneZoom);
    } else {
      widget.onSendZoomCommand(opticalZoom);
    }
  }

  // Handle pointer signal (Mouse Wheel) for Desktop
  void _handlePointerSignal(PointerSignalEvent event) {
    if (event is PointerScrollEvent) {
      final RenderBox? renderBox = context.findRenderObject() as RenderBox?;
      if (renderBox == null) return;

      final localPoint = renderBox.globalToLocal(event.position);

      double scaleChange = event.scrollDelta.dy > 0 ? 0.9 : 1.1;
      double newTotalZoom = (_localZoom * scaleChange).clamp(
        _minTotalZoom,
        _maxTotalZoom,
      );

      double limit = widget.phoneMaxZoom;
      if (widget.isDigitalZoomOverride) limit = widget.lockedPhoneZoom;

      double opticalZoom = newTotalZoom.clamp(1.0, limit);

      // Current State
      double oldVisualScale = 1.0;
      if (_localZoom > limit) {
        oldVisualScale = _localZoom / limit;
      }

      // New State
      double newVisualScale = 1.0;
      if (newTotalZoom > limit) {
        newVisualScale = newTotalZoom / limit;
      }

      Offset newPanOffset = Offset.zero;

      if (newVisualScale > 1.0) {
        // T_new = P - ((P - T_old) / S_old) * S_new
        final contentPoint = (localPoint - _panOffset) / oldVisualScale;
        newPanOffset = localPoint - (contentPoint * newVisualScale);
      }

      // Clamp
      if (_viewportSize != Size.zero && newVisualScale > 1.0) {
        final scaledW = _viewportSize.width * newVisualScale;
        final scaledH = _viewportSize.height * newVisualScale;

        final minX = _viewportSize.width - scaledW;
        final minY = _viewportSize.height - scaledH;

        newPanOffset = Offset(
          newPanOffset.dx.clamp(minX, 0.0),
          newPanOffset.dy.clamp(minY, 0.0),
        );
      } else {
        newPanOffset = Offset.zero;
      }

      widget.onTransformChanged(newTotalZoom, newPanOffset, _viewportSize);
      widget.onSendZoomCommand(opticalZoom);

      setState(() {
        _panOffset = newPanOffset;
        _localZoom = newTotalZoom;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    // Calculate visual matrix for rendering
    double limit = widget.phoneMaxZoom;
    if (widget.isDigitalZoomOverride) limit = widget.lockedPhoneZoom;

    // Current State
    double totalZoom = _localZoom;

    // Digital Scale Part
    double visualScale = 1.0;
    if (totalZoom > limit) {
      visualScale = totalZoom / limit;
    }

    // Matrix
    final translation = Matrix4.translationValues(
      _panOffset.dx,
      _panOffset.dy,
      0.0,
    );
    final scale = Matrix4.diagonal3Values(visualScale, visualScale, 1.0);
    final matrix = translation..multiply(scale);

    return LayoutBuilder(
      builder: (context, constraints) {
        if (_viewportSize != constraints.biggest) {
          _viewportSize = constraints.biggest;
        }

        return Listener(
          onPointerSignal: _handlePointerSignal,
          child: GestureDetector(
            onScaleStart: _onScaleStart,
            onScaleUpdate: _onScaleUpdate,
            child: ClipRect(
              child: Transform(
                transform: matrix,
                alignment: Alignment.topLeft,
                child: widget.child,
              ),
            ),
          ),
        );
      },
    );
  }
}
