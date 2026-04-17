import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../models/graph_node_info.dart';

class GraphHistorySparkline extends StatelessWidget {
  final List<GraphHistoryTimelineEntry> timeline;
  final int selectedIndex;
  final int peakIndex;

  const GraphHistorySparkline({
    super.key,
    required this.timeline,
    required this.selectedIndex,
    required this.peakIndex,
  });

  @override
  Widget build(BuildContext context) {
    if (timeline.length < 2) {
      return const SizedBox(height: 22);
    }

    return SizedBox(
      height: 26,
      child: CustomPaint(
        painter: _GraphHistorySparklinePainter(
          timeline: timeline,
          selectedIndex: selectedIndex,
          peakIndex: peakIndex,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _GraphHistorySparklinePainter extends CustomPainter {
  final List<GraphHistoryTimelineEntry> timeline;
  final int selectedIndex;
  final int peakIndex;

  const _GraphHistorySparklinePainter({
    required this.timeline,
    required this.selectedIndex,
    required this.peakIndex,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (timeline.isEmpty || size.isEmpty) return;

    final chartRect = Rect.fromLTWH(0, 2, size.width, math.max(1, size.height - 4));
    final minCount = timeline
        .map((entry) => entry.nodeCount)
        .reduce(math.min)
        .toDouble();
    final maxCount = timeline
        .map((entry) => entry.nodeCount)
        .reduce(math.max)
        .toDouble();
    final countRange = math.max(1.0, maxCount - minCount);
    final xStep = timeline.length == 1 ? 0.0 : chartRect.width / (timeline.length - 1);

    final points = <Offset>[];
    for (int i = 0; i < timeline.length; i++) {
      final normalizedY = (timeline[i].nodeCount - minCount) / countRange;
      points.add(
        Offset(
          chartRect.left + xStep * i,
          chartRect.bottom - normalizedY * chartRect.height,
        ),
      );
    }

    final baselinePaint = Paint()
      ..color = Colors.white24
      ..strokeWidth = 1;
    canvas.drawLine(
      Offset(chartRect.left, chartRect.bottom),
      Offset(chartRect.right, chartRect.bottom),
      baselinePaint,
    );

    final areaPath = Path()..moveTo(points.first.dx, chartRect.bottom);
    for (final point in points) {
      areaPath.lineTo(point.dx, point.dy);
    }
    areaPath
      ..lineTo(points.last.dx, chartRect.bottom)
      ..close();
    canvas.drawPath(
      areaPath,
      Paint()
        ..shader = const LinearGradient(
          begin: Alignment.topCenter,
          end: Alignment.bottomCenter,
          colors: [Color(0x6648CFAE), Color(0x0048CFAE)],
        ).createShader(chartRect),
    );

    final linePath = Path()..moveTo(points.first.dx, points.first.dy);
    for (int i = 1; i < points.length; i++) {
      linePath.lineTo(points[i].dx, points[i].dy);
    }
    canvas.drawPath(
      linePath,
      Paint()
        ..color = const Color(0xFF48CFAE)
        ..strokeWidth = 2
        ..style = PaintingStyle.stroke,
    );

    if (peakIndex >= 0 && peakIndex < points.length) {
      final peakPoint = points[peakIndex];
      canvas.drawCircle(
        peakPoint,
        4,
        Paint()..color = const Color(0xFFFFB347),
      );
      canvas.drawCircle(
        peakPoint,
        7,
        Paint()
          ..color = const Color(0x33FFB347)
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2,
      );
    }

    if (selectedIndex >= 0 && selectedIndex < points.length) {
      final selectedPoint = points[selectedIndex];
      canvas.drawLine(
        Offset(selectedPoint.dx, chartRect.top),
        Offset(selectedPoint.dx, chartRect.bottom),
        Paint()
          ..color = const Color(0x99FFFFFF)
          ..strokeWidth = 1,
      );
      canvas.drawCircle(
        selectedPoint,
        4,
        Paint()..color = Colors.white,
      );
      canvas.drawCircle(
        selectedPoint,
        7,
        Paint()
          ..color = const Color(0x66FFFFFF)
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2,
      );
    }
  }

  @override
  bool shouldRepaint(covariant _GraphHistorySparklinePainter oldDelegate) {
    return oldDelegate.timeline != timeline ||
        oldDelegate.selectedIndex != selectedIndex ||
        oldDelegate.peakIndex != peakIndex;
  }
}