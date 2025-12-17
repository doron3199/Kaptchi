import 'package:flutter/material.dart';
import 'dart:typed_data';

/// A resizable and croppable overlay widget with corner and edge handles.
/// - Drag corners to resize proportionally (scale the whole image)
/// - Drag edges to crop (cut/trim the image from that edge)
/// - Drag body to move
/// - Double-tap to remove
class ResizableOverlay extends StatefulWidget {
  final Uint8List imageBytes;
  final Offset initialPosition;
  final double initialWidth;
  final double initialHeight;
  final VoidCallback onRemove;
  final Function(Offset position, double width, double height) onChanged;

  const ResizableOverlay({
    super.key,
    required this.imageBytes,
    required this.initialPosition,
    required this.initialWidth,
    required this.initialHeight,
    required this.onRemove,
    required this.onChanged,
  });

  @override
  State<ResizableOverlay> createState() => _ResizableOverlayState();
}

class _ResizableOverlayState extends State<ResizableOverlay> {
  late Offset _position;
  late double _baseWidth; // Original image display width (before crop)
  late double _baseHeight; // Original image display height (before crop)

  // Crop insets - how much to clip from each edge
  double _cropLeft = 0;
  double _cropTop = 0;
  double _cropRight = 0;
  double _cropBottom = 0;

  // Handle sizes - larger for easier touch
  static const double _handleTouchSize = 32;
  static const double _handleVisualSize = 10;
  static const double _minSize = 30; // Minimum visible size

  @override
  void initState() {
    super.initState();
    _position = widget.initialPosition;
    _baseWidth = widget.initialWidth;
    _baseHeight = widget.initialHeight;
  }

  // Get visible dimensions after crop
  double get _visibleWidth => _baseWidth - _cropLeft - _cropRight;
  double get _visibleHeight => _baseHeight - _cropTop - _cropBottom;

  void _notifyChange() {
    widget.onChanged(_position, _visibleWidth, _visibleHeight);
  }

  @override
  Widget build(BuildContext context) {
    return Positioned(
      left: _position.dx,
      top: _position.dy,
      child: SizedBox(
        width: _visibleWidth + _handleTouchSize,
        height: _visibleHeight + _handleTouchSize,
        child: Stack(
          clipBehavior: Clip.none,
          children: [
            // Main image (draggable body) - cropped, NO opacity
            Positioned(
              left: _handleTouchSize / 2,
              top: _handleTouchSize / 2,
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onPanUpdate: (details) {
                  setState(() {
                    _position += details.delta;
                    _notifyChange();
                  });
                },
                onDoubleTap: widget.onRemove,
                child: Container(
                  width: _visibleWidth,
                  height: _visibleHeight,
                  decoration: BoxDecoration(
                    border: Border.all(color: Colors.white, width: 2),
                    boxShadow: [
                      BoxShadow(
                        color: Colors.black.withValues(alpha: 0.3),
                        blurRadius: 4,
                        offset: const Offset(2, 2),
                      ),
                    ],
                  ),
                  child: ClipRect(
                    child: OverflowBox(
                      alignment: Alignment.topLeft,
                      maxWidth: _baseWidth,
                      maxHeight: _baseHeight,
                      child: Transform.translate(
                        offset: Offset(-_cropLeft, -_cropTop),
                        child: Image.memory(
                          widget.imageBytes,
                          width: _baseWidth,
                          height: _baseHeight,
                          fit: BoxFit.fill,
                          cacheWidth: 800,
                          filterQuality: FilterQuality.medium,
                          gaplessPlayback: true,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),

            // Corner handles (for proportional resize)
            _buildCornerHandle(Corner.topLeft),
            _buildCornerHandle(Corner.topRight),
            _buildCornerHandle(Corner.bottomLeft),
            _buildCornerHandle(Corner.bottomRight),

            // Edge handles (for cropping)
            _buildEdgeHandle(Edge.top),
            _buildEdgeHandle(Edge.bottom),
            _buildEdgeHandle(Edge.left),
            _buildEdgeHandle(Edge.right),
          ],
        ),
      ),
    );
  }

  Widget _buildCornerHandle(Corner corner) {
    double left = 0;
    double top = 0;
    MouseCursor cursor = SystemMouseCursors.basic;

    switch (corner) {
      case Corner.topLeft:
        left = 0;
        top = 0;
        cursor = SystemMouseCursors.resizeUpLeft;
      case Corner.topRight:
        left = _visibleWidth;
        top = 0;
        cursor = SystemMouseCursors.resizeUpRight;
      case Corner.bottomLeft:
        left = 0;
        top = _visibleHeight;
        cursor = SystemMouseCursors.resizeDownLeft;
      case Corner.bottomRight:
        left = _visibleWidth;
        top = _visibleHeight;
        cursor = SystemMouseCursors.resizeDownRight;
    }

    return Positioned(
      left: left,
      top: top,
      child: MouseRegion(
        cursor: cursor,
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onPanUpdate: (details) => _handleCornerDrag(corner, details.delta),
          child: Container(
            width: _handleTouchSize,
            height: _handleTouchSize,
            alignment: Alignment.center,
            child: Container(
              width: _handleVisualSize,
              height: _handleVisualSize,
              decoration: BoxDecoration(
                color: Colors.white,
                border: Border.all(color: Colors.blue, width: 2),
                shape: BoxShape.circle,
              ),
            ),
          ),
        ),
      ),
    );
  }

  void _handleCornerDrag(Corner corner, Offset delta) {
    setState(() {
      final aspectRatio = _baseWidth / _baseHeight;

      switch (corner) {
        case Corner.topLeft:
          // Scale from top-left
          final avgDelta = (delta.dx + delta.dy) / 2;
          final oldWidth = _baseWidth;
          final newWidth = (_baseWidth - avgDelta).clamp(_minSize, 1000.0);
          final newHeight = newWidth / aspectRatio;
          final scale = newWidth / oldWidth;

          _position += Offset(_baseWidth - newWidth, _baseHeight - newHeight);
          _baseWidth = newWidth;
          _baseHeight = newHeight;
          _cropLeft *= scale;
          _cropTop *= scale;
          _cropRight *= scale;
          _cropBottom *= scale;

        case Corner.topRight:
          final avgDelta = (delta.dx - delta.dy) / 2;
          final oldWidth = _baseWidth;
          final newWidth = (_baseWidth + avgDelta).clamp(_minSize, 1000.0);
          final newHeight = newWidth / aspectRatio;
          final scale = newWidth / oldWidth;

          _position += Offset(0, _baseHeight - newHeight);
          _baseWidth = newWidth;
          _baseHeight = newHeight;
          _cropLeft *= scale;
          _cropTop *= scale;
          _cropRight *= scale;
          _cropBottom *= scale;

        case Corner.bottomLeft:
          final avgDelta = (-delta.dx + delta.dy) / 2;
          final oldWidth = _baseWidth;
          final newWidth = (_baseWidth + avgDelta).clamp(_minSize, 1000.0);
          final newHeight = newWidth / aspectRatio;
          final scale = newWidth / oldWidth;

          _position += Offset(_baseWidth - newWidth, 0);
          _baseWidth = newWidth;
          _baseHeight = newHeight;
          _cropLeft *= scale;
          _cropTop *= scale;
          _cropRight *= scale;
          _cropBottom *= scale;

        case Corner.bottomRight:
          final avgDelta = (delta.dx + delta.dy) / 2;
          final oldWidth = _baseWidth;
          final newWidth = (_baseWidth + avgDelta).clamp(_minSize, 1000.0);
          final newHeight = newWidth / aspectRatio;
          final scale = newWidth / oldWidth;

          _baseWidth = newWidth;
          _baseHeight = newHeight;
          _cropLeft *= scale;
          _cropTop *= scale;
          _cropRight *= scale;
          _cropBottom *= scale;
      }

      _clampCropValues();
      _notifyChange();
    });
  }

  Widget _buildEdgeHandle(Edge edge) {
    double left = 0;
    double top = 0;
    double width = _handleTouchSize;
    double height = _handleTouchSize;
    MouseCursor cursor = SystemMouseCursors.basic;

    switch (edge) {
      case Edge.top:
        left = _visibleWidth / 2;
        top = 0;
        width = _handleTouchSize * 2;
        cursor = SystemMouseCursors.resizeUp;
      case Edge.bottom:
        left = _visibleWidth / 2;
        top = _visibleHeight;
        width = _handleTouchSize * 2;
        cursor = SystemMouseCursors.resizeDown;
      case Edge.left:
        left = 0;
        top = _visibleHeight / 2;
        height = _handleTouchSize * 2;
        cursor = SystemMouseCursors.resizeLeft;
      case Edge.right:
        left = _visibleWidth;
        top = _visibleHeight / 2;
        height = _handleTouchSize * 2;
        cursor = SystemMouseCursors.resizeRight;
    }

    return Positioned(
      left: left,
      top: top,
      child: MouseRegion(
        cursor: cursor,
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onPanUpdate: (details) => _handleEdgeDrag(edge, details.delta),
          child: Container(
            width: width,
            height: height,
            alignment: Alignment.center,
            child: Container(
              width: edge == Edge.top || edge == Edge.bottom
                  ? _handleTouchSize
                  : _handleVisualSize / 2,
              height: edge == Edge.left || edge == Edge.right
                  ? _handleTouchSize
                  : _handleVisualSize / 2,
              decoration: BoxDecoration(
                color: Colors.white,
                border: Border.all(color: Colors.orange, width: 2),
                borderRadius: BorderRadius.circular(2),
              ),
            ),
          ),
        ),
      ),
    );
  }

  void _handleEdgeDrag(Edge edge, Offset delta) {
    setState(() {
      switch (edge) {
        case Edge.left:
          // Store old value to calculate actual change
          final oldCropLeft = _cropLeft;
          _cropLeft = (_cropLeft + delta.dx).clamp(
            0.0,
            _baseWidth - _cropRight - _minSize,
          );
          // Move position by the ACTUAL change (respects clamp)
          final actualChange = _cropLeft - oldCropLeft;
          _position += Offset(actualChange, 0);

        case Edge.right:
          // Dragging right edge: positive delta = expand, negative = crop
          _cropRight = (_cropRight - delta.dx).clamp(
            0.0,
            _baseWidth - _cropLeft - _minSize,
          );

        case Edge.top:
          // Store old value to calculate actual change
          final oldCropTop = _cropTop;
          _cropTop = (_cropTop + delta.dy).clamp(
            0.0,
            _baseHeight - _cropBottom - _minSize,
          );
          // Move position by the ACTUAL change (respects clamp)
          final actualChange = _cropTop - oldCropTop;
          _position += Offset(0, actualChange);

        case Edge.bottom:
          // Dragging bottom edge: positive delta = expand, negative = crop
          _cropBottom = (_cropBottom - delta.dy).clamp(
            0.0,
            _baseHeight - _cropTop - _minSize,
          );
      }

      _clampCropValues();
      _notifyChange();
    });
  }

  void _clampCropValues() {
    // Ensure minimum visible size
    if (_visibleWidth < _minSize) {
      final diff = _minSize - _visibleWidth;
      _cropRight = (_cropRight - diff / 2).clamp(0.0, double.infinity);
      _cropLeft = (_cropLeft - diff / 2).clamp(0.0, double.infinity);
    }
    if (_visibleHeight < _minSize) {
      final diff = _minSize - _visibleHeight;
      _cropBottom = (_cropBottom - diff / 2).clamp(0.0, double.infinity);
      _cropTop = (_cropTop - diff / 2).clamp(0.0, double.infinity);
    }
  }
}

enum Corner { topLeft, topRight, bottomLeft, bottomRight }

enum Edge { top, bottom, left, right }
