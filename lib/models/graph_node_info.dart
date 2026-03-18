import 'dart:ui';

class GraphNodeInfo {
  final int id;
  final Rect bboxCanvas;
  final Offset centroid;
  final double area;
  final double absenceScore;
  final int lastSeenFrame;
  final int createdFrame;
  final int neighborCount;
  final Offset canvasOrigin;
  final int matchDistance; // BFS hop distance from matched blob (0=matched, 1=neighbor, -1=not seen)
  final List<Offset> contour; // Contour points in canvas coordinates

  const GraphNodeInfo({
    required this.id,
    required this.bboxCanvas,
    required this.centroid,
    required this.area,
    required this.absenceScore,
    required this.lastSeenFrame,
    required this.createdFrame,
    required this.neighborCount,
    required this.canvasOrigin,
    this.matchDistance = -1,
    this.contour = const [],
  });

  /// Returns a copy with updated centroid and bbox (for drag in Flutter only).
  GraphNodeInfo copyWithPosition({required Offset newCentroid}) {
    final dx = newCentroid.dx - centroid.dx;
    final dy = newCentroid.dy - centroid.dy;
    return GraphNodeInfo(
      id: id,
      bboxCanvas: bboxCanvas.shift(Offset(dx, dy)),
      centroid: newCentroid,
      area: area,
      absenceScore: absenceScore,
      lastSeenFrame: lastSeenFrame,
      createdFrame: createdFrame,
      neighborCount: neighborCount,
      canvasOrigin: canvasOrigin,
      matchDistance: matchDistance,
      contour: contour.map((p) => Offset(p.dx + dx, p.dy + dy)).toList(),
    );
  }

  Rect get bboxRelative => bboxCanvas.shift(-canvasOrigin);
}

class NodeComparison {
  final double shapeDistance;
  final double centroidDistance;
  final double bboxIntersectionArea;
  final double andOverlapPixels;
  final double maskOverlapRatio;

  const NodeComparison({
    required this.shapeDistance,
    required this.centroidDistance,
    required this.bboxIntersectionArea,
    required this.andOverlapPixels,
    required this.maskOverlapRatio,
  });
}

class GraphSnapshotComparison {
  final double shapeDistance;
  final double widthRatio;
  final double heightRatio;
  final double longEdgeSimilarity;
  final double shortEdgeSimilarity;
  final double averageEdgeSimilarity;

  const GraphSnapshotComparison({
    required this.shapeDistance,
    required this.widthRatio,
    required this.heightRatio,
    required this.longEdgeSimilarity,
    required this.shortEdgeSimilarity,
    required this.averageEdgeSimilarity,
  });
}
