import 'dart:async';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../services/native_camera_service.dart';

class CanvasOverviewBar extends StatefulWidget {
  final double panX;
  final double panY;
  final double zoom;
  final int resetVersion;
  final ValueChanged<({double panX, double panY, double zoom})>
  onViewportChanged;

  const CanvasOverviewBar({
    super.key,
    required this.panX,
    required this.panY,
    required this.zoom,
    required this.resetVersion,
    required this.onViewportChanged,
  });

  @override
  State<CanvasOverviewBar> createState() => _CanvasOverviewBarState();
}

class _CanvasOverviewBarState extends State<CanvasOverviewBar> {
  static const int _previewPixelHeight = 96;
  static const Duration _refreshInterval = Duration(milliseconds: 900);
  static const Duration _interactionQuietPeriod = Duration(milliseconds: 250);

  Timer? _refreshTimer;
  ui.Image? _overviewImage;
  Size _canvasSize = Size.zero;
  Size _frameSize = const Size(16, 9);
  bool _isRefreshing = false;
  DateTime _lastViewportChange = DateTime.fromMillisecondsSinceEpoch(0);

  @override
  void initState() {
    super.initState();
    _refreshOverview();
    _refreshTimer = Timer.periodic(_refreshInterval, (_) => _refreshOverview());
  }

  @override
  void dispose() {
    _refreshTimer?.cancel();
    _overviewImage?.dispose();
    super.dispose();
  }

  @override
  void didUpdateWidget(covariant CanvasOverviewBar oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.panX != widget.panX ||
        oldWidget.panY != widget.panY ||
        oldWidget.zoom != widget.zoom) {
      _lastViewportChange = DateTime.now();
    }
    if (oldWidget.resetVersion != widget.resetVersion) {
      final previous = _overviewImage;
      _overviewImage = null;
      _canvasSize = Size.zero;
      previous?.dispose();
      _refreshOverview();
    }
  }

  Future<void> _refreshOverview() async {
    if (_isRefreshing || !mounted) return;
    _isRefreshing = true;

    try {
      if (DateTime.now().difference(_lastViewportChange) <
          _interactionQuietPeriod) {
        return;
      }

      final service = NativeCameraService();
      final rawCanvasSize = service.getPanoramaCanvasSize();
      final overviewAspect = rawCanvasSize.width > 0 && rawCanvasSize.height > 0
          ? rawCanvasSize.width / rawCanvasSize.height
          : (_frameSize.height > 0
                ? _frameSize.width / _frameSize.height
                : 16 / 9);
      final previewPixelWidth = (_previewPixelHeight * overviewAspect)
          .clamp(1.0, 8192.0)
          .round();

      final bytes = service.getCanvasOverviewRgba(
        previewPixelWidth,
        _previewPixelHeight,
      );
      final frameWidth = service.getFrameWidth().toDouble();
      final frameHeight = service.getFrameHeight().toDouble();

      ui.Image? nextImage;
      if (bytes != null) {
        nextImage = await _decodeRgbaImage(
          bytes,
          previewPixelWidth,
          _previewPixelHeight,
        );
      }

      if (!mounted) {
        nextImage?.dispose();
        return;
      }

      setState(() {
        if (nextImage != null) {
          final previous = _overviewImage;
          _overviewImage = nextImage;
          previous?.dispose();
          if (rawCanvasSize.width > 0 && rawCanvasSize.height > 0) {
            _canvasSize = rawCanvasSize;
          }
        }
        if (frameWidth > 0 && frameHeight > 0) {
          _frameSize = Size(frameWidth, frameHeight);
        }
      });
    } finally {
      _isRefreshing = false;
    }
  }

  Future<ui.Image> _decodeRgbaImage(
    Uint8List bytes,
    int width,
    int height,
  ) async {
    final completer = Completer<ui.Image>();
    ui.decodeImageFromPixels(
      bytes,
      width,
      height,
      ui.PixelFormat.rgba8888,
      completer.complete,
    );
    return completer.future;
  }

  ({double roiW, double roiH, double maxCx, double maxCy, double cx, double cy})
  _viewportMetrics() {
    final cw = _canvasSize.width <= 0 ? 1.0 : _canvasSize.width;
    final ch = _canvasSize.height <= 0 ? 1.0 : _canvasSize.height;
    final viewAspect = _frameSize.height <= 0
        ? 16 / 9
        : _frameSize.width / _frameSize.height;

    var roiH = ch / widget.zoom;
    var roiW = roiH * viewAspect;
    if (roiW > cw) {
      roiW = cw;
      roiH = roiW / viewAspect;
    }
    if (roiH > ch) {
      roiH = ch;
      roiW = roiH * viewAspect;
    }

    final maxCx = (cw - roiW).clamp(0.0, double.infinity);
    final maxCy = (ch - roiH).clamp(0.0, double.infinity);
    final cx = (widget.panX * maxCx).clamp(0.0, maxCx);
    final cy = (widget.panY * maxCy).clamp(0.0, maxCy);

    return (roiW: roiW, roiH: roiH, maxCx: maxCx, maxCy: maxCy, cx: cx, cy: cy);
  }

  Rect _contentRect(Size size) {
    if (size.width <= 0 || size.height <= 0) {
      return Rect.zero;
    }

    if (_canvasSize.width <= 0 || _canvasSize.height <= 0) {
      return Offset.zero & size;
    }

    final srcAspect = _canvasSize.width / _canvasSize.height;
    final dstAspect = size.width / size.height;

    if (srcAspect > dstAspect) {
      final drawHeight = size.width / srcAspect;
      return Rect.fromLTWH(
        0,
        (size.height - drawHeight) / 2,
        size.width,
        drawHeight,
      );
    }

    final drawWidth = size.height * srcAspect;
    return Rect.fromLTWH(
      (size.width - drawWidth) / 2,
      0,
      drawWidth,
      size.height,
    );
  }

  void _updatePanFromOverview(Offset localPosition, Size size) {
    if (_canvasSize.width <= 0) return;

    final contentRect = _contentRect(size);
    if (contentRect.width <= 0 || contentRect.height <= 0) return;

    final metrics = _viewportMetrics();
    if (metrics.maxCx <= 0) return;

    final localX = (localPosition.dx - contentRect.left).clamp(
      0.0,
      contentRect.width,
    );
    final targetCenterX = (localX / contentRect.width) * _canvasSize.width;
    final nextCx = (targetCenterX - (metrics.roiW / 2)).clamp(
      0.0,
      metrics.maxCx,
    );
    final nextPanX = nextCx / metrics.maxCx;

    widget.onViewportChanged((
      panX: nextPanX,
      panY: widget.panY,
      zoom: widget.zoom,
    ));
  }

  @override
  Widget build(BuildContext context) {
    final metrics = _viewportMetrics();

    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          height: 56,
          child: LayoutBuilder(
            builder: (context, constraints) {
              final size = constraints.biggest;
              final contentRect = _contentRect(size);
              final overlayRect = Rect.fromLTWH(
                contentRect.left +
                    ((metrics.cx /
                            (_canvasSize.width <= 0 ? 1 : _canvasSize.width)) *
                        contentRect.width),
                contentRect.top +
                    ((metrics.cy /
                            (_canvasSize.height <= 0
                                ? 1
                                : _canvasSize.height)) *
                        contentRect.height),
                (_canvasSize.width <= 0
                        ? 1.0
                        : metrics.roiW / _canvasSize.width) *
                    contentRect.width,
                (_canvasSize.height <= 0
                        ? 1.0
                        : metrics.roiH / _canvasSize.height) *
                    contentRect.height,
              );

              return GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTapDown: (details) =>
                    _updatePanFromOverview(details.localPosition, size),
                onHorizontalDragUpdate: (details) =>
                    _updatePanFromOverview(details.localPosition, size),
                child: ColoredBox(
                  color: Colors.white,
                  child: Stack(
                    fit: StackFit.expand,
                    children: [
                      if (_overviewImage != null)
                        Positioned.fromRect(
                          rect: contentRect,
                          child: RawImage(
                            image: _overviewImage,
                            fit: BoxFit.fill,
                            filterQuality: FilterQuality.low,
                          ),
                        ),
                      if (_overviewImage != null &&
                          overlayRect.width > 0 &&
                          overlayRect.height > 0)
                        Positioned.fromRect(
                          rect: overlayRect,
                          child: IgnorePointer(
                            child: Container(
                              decoration: BoxDecoration(
                                color: Colors.blueAccent.withValues(
                                  alpha: 0.14,
                                ),
                                border: Border.all(
                                  color: Colors.blueAccent,
                                  width: 2,
                                ),
                              ),
                            ),
                          ),
                        ),
                    ],
                  ),
                ),
              );
            },
          ),
        ),
        const SizedBox(
          height: 1,
          width: double.infinity,
          child: ColoredBox(color: Colors.black),
        ),
      ],
    );
  }
}
