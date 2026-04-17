import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../models/graph_node_info.dart';

class VectorGraphSnapshotView extends StatelessWidget {
  final List<GraphNodeInfo> nodes;
  final Rect? canvasBounds;
  final Color backgroundColor;
  final Color shapeColor;
  final Color centroidColor;
  final Color labelColor;
  final bool showLabels;
  final Widget? emptyChild;

  const VectorGraphSnapshotView({
    super.key,
    required this.nodes,
    required this.canvasBounds,
    this.backgroundColor = Colors.white,
    this.shapeColor = Colors.black,
    this.centroidColor = Colors.red,
    this.labelColor = const Color(0xFF1F2937),
    this.showLabels = false,
    this.emptyChild,
  });

  @override
  Widget build(BuildContext context) {
    if (canvasBounds == null || canvasBounds!.isEmpty || nodes.isEmpty) {
      return ColoredBox(
        color: backgroundColor,
        child:
            emptyChild ??
            const Center(
              child: Text(
                'No graph snapshot selected',
                style: TextStyle(color: Colors.black54),
              ),
            ),
      );
    }

    return ColoredBox(
      color: backgroundColor,
      child: CustomPaint(
        painter: VectorGraphSnapshotPainter(
          nodes: nodes,
          canvasBounds: canvasBounds!,
          backgroundColor: backgroundColor,
          shapeColor: shapeColor,
          centroidColor: centroidColor,
          labelColor: labelColor,
          showLabels: showLabels,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class VectorGraphSnapshotPainter extends CustomPainter {
  final List<GraphNodeInfo> nodes;
  final Rect canvasBounds;
  final Color backgroundColor;
  final Color shapeColor;
  final Color centroidColor;
  final Color labelColor;
  final bool showLabels;

  VectorGraphSnapshotPainter({
    required this.nodes,
    required this.canvasBounds,
    required this.backgroundColor,
    required this.shapeColor,
    required this.centroidColor,
    required this.labelColor,
    required this.showLabels,
  });

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(
      Offset.zero & size,
      Paint()
        ..color = backgroundColor
        ..style = PaintingStyle.fill,
    );

    if (nodes.isEmpty || canvasBounds.isEmpty || size.isEmpty) return;

    final transform = _GraphCanvasTransform.fromBounds(canvasBounds, size);
    final fillPaint = Paint()
      ..color = shapeColor
      ..style = PaintingStyle.fill;
    final strokePaint = Paint()
      ..color = shapeColor.withAlpha(180)
      ..strokeWidth = 1.0
      ..style = PaintingStyle.stroke;
    final centerPaint = Paint()
      ..color = centroidColor
      ..style = PaintingStyle.fill;
    final textPainter = TextPainter(textDirection: TextDirection.ltr);

    for (final node in nodes) {
      if (node.contour.length >= 3) {
        final path = ui.Path();
        final first = transform.canvasToScreen(node.contour.first);
        path.moveTo(first.dx, first.dy);
        for (int i = 1; i < node.contour.length; i++) {
          final point = transform.canvasToScreen(node.contour[i]);
          path.lineTo(point.dx, point.dy);
        }
        path.close();
        canvas.drawPath(path, fillPaint);
      } else {
        canvas.drawRect(transform.canvasRectToScreen(node.bboxCanvas), strokePaint);
      }

      final screenCentroid = transform.canvasToScreen(node.centroid);
      canvas.drawCircle(screenCentroid, 2.5, centerPaint);

      if (!showLabels) continue;
      textPainter.text = TextSpan(
        text: '${node.id}',
        style: TextStyle(
          color: labelColor,
          fontSize: 10,
          fontWeight: FontWeight.w600,
        ),
      );
      textPainter.layout();
      textPainter.paint(canvas, screenCentroid + const Offset(4, -12));
    }
  }

  @override
  bool shouldRepaint(covariant VectorGraphSnapshotPainter oldDelegate) {
    return oldDelegate.nodes != nodes ||
        oldDelegate.canvasBounds != canvasBounds ||
        oldDelegate.backgroundColor != backgroundColor ||
        oldDelegate.shapeColor != shapeColor ||
        oldDelegate.centroidColor != centroidColor ||
        oldDelegate.labelColor != labelColor ||
        oldDelegate.showLabels != showLabels;
  }
}

class _GraphCanvasTransform {
  final Rect bounds;
  final double scale;
  final Offset contentOffset;

  const _GraphCanvasTransform({
    required this.bounds,
    required this.scale,
    required this.contentOffset,
  });

  factory _GraphCanvasTransform.fromBounds(Rect bounds, Size viewportSize) {
    final safeWidth = bounds.width <= 0 ? 1.0 : bounds.width;
    final safeHeight = bounds.height <= 0 ? 1.0 : bounds.height;
    final scale = (viewportSize.width / safeWidth)
        .clamp(0.0001, double.infinity)
        .toDouble();
    final fittedScale = [
      scale,
      (viewportSize.height / safeHeight).clamp(0.0001, double.infinity).toDouble(),
    ].reduce((a, b) => a < b ? a : b);
    final contentSize = Size(safeWidth * fittedScale, safeHeight * fittedScale);
    final contentOffset = Offset(
      (viewportSize.width - contentSize.width) / 2,
      (viewportSize.height - contentSize.height) / 2,
    );
    return _GraphCanvasTransform(
      bounds: bounds,
      scale: fittedScale,
      contentOffset: contentOffset,
    );
  }

  Offset canvasToScreen(Offset point) {
    return Offset(
      contentOffset.dx + (point.dx - bounds.left) * scale,
      contentOffset.dy + (point.dy - bounds.top) * scale,
    );
  }

  Rect canvasRectToScreen(Rect rect) {
    final topLeft = canvasToScreen(rect.topLeft);
    return Rect.fromLTWH(
      topLeft.dx,
      topLeft.dy,
      rect.width * scale,
      rect.height * scale,
    );
  }
}

Future<Uint8List> renderGraphSnapshotPng({
  required List<GraphNodeInfo> nodes,
  required Rect? canvasBounds,
  required int width,
  required int height,
  Color backgroundColor = Colors.white,
  Color shapeColor = Colors.black,
  Color centroidColor = Colors.red,
  Color labelColor = const Color(0xFF1F2937),
  bool showLabels = false,
}) async {
  final recorder = ui.PictureRecorder();
  final canvas = Canvas(recorder);
  final size = Size(width.toDouble(), height.toDouble());

  VectorGraphSnapshotPainter(
    nodes: nodes,
    canvasBounds: canvasBounds ?? Rect.zero,
    backgroundColor: backgroundColor,
    shapeColor: shapeColor,
    centroidColor: centroidColor,
    labelColor: labelColor,
    showLabels: showLabels,
  ).paint(canvas, size);

  final picture = recorder.endRecording();
  final image = await picture.toImage(width, height);
  final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
  picture.dispose();
  image.dispose();
  if (bytes == null) {
    throw StateError('Failed to render graph snapshot PNG');
  }
  return bytes.buffer.asUint8List();
}
