import 'dart:typed_data';
import 'dart:ui';

class GraphNodeInfo {
  static const int duplicateReasonPositionalOverlap = 1 << 0;
  static const int duplicateReasonCentroidIou = 1 << 1;
  static const int duplicateReasonBboxIou = 1 << 2;
  static const int duplicateReasonShapeDifference = 1 << 3;

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
  final bool isUserLocked; // User-edited — immune to pipeline changes
  final bool isDuplicateDebug;
  final int duplicatePartnerId;
  final double duplicatePositionalOverlap;
  final double duplicateCentroidIou;
  final double duplicateBboxIou;
  final double duplicateShapeDifference;
  final int duplicateReasonMask;

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
    this.isUserLocked = false,
    this.isDuplicateDebug = false,
    this.duplicatePartnerId = -1,
    this.duplicatePositionalOverlap = 0,
    this.duplicateCentroidIou = 0,
    this.duplicateBboxIou = 0,
    this.duplicateShapeDifference = 1,
    this.duplicateReasonMask = 0,
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
      isUserLocked: isUserLocked,
      isDuplicateDebug: isDuplicateDebug,
      duplicatePartnerId: duplicatePartnerId,
      duplicatePositionalOverlap: duplicatePositionalOverlap,
      duplicateCentroidIou: duplicateCentroidIou,
      duplicateBboxIou: duplicateBboxIou,
      duplicateShapeDifference: duplicateShapeDifference,
      duplicateReasonMask: duplicateReasonMask,
    );
  }

  Rect get bboxRelative => bboxCanvas.shift(-canvasOrigin);

  List<String> get duplicateReasonLabels {
    final labels = <String>[];
    if ((duplicateReasonMask & duplicateReasonPositionalOverlap) != 0) {
      labels.add('Pos overlap');
    }
    if ((duplicateReasonMask & duplicateReasonCentroidIou) != 0) {
      labels.add('Centroid IoU');
    }
    if ((duplicateReasonMask & duplicateReasonBboxIou) != 0) {
      labels.add('BBox IoU');
    }
    if ((duplicateReasonMask & duplicateReasonShapeDifference) != 0) {
      labels.add('Shape diff');
    }
    return labels;
  }
}

class GraphHardEdge {
  final int firstNodeId;
  final int secondNodeId;

  const GraphHardEdge({
    required this.firstNodeId,
    required this.secondNodeId,
  });
}

class NodeComparison {
  static const int duplicateReasonPositionalOverlap = 1 << 0;
  static const int duplicateReasonCentroidIou = 1 << 1;
  static const int duplicateReasonBboxIou = 1 << 2;
  static const int duplicateReasonShapeDifference = 1 << 3;

  final double shapeDistance;
  final double centroidDistance;
  final double bboxIntersectionArea;
  final double andOverlapPixels;
  final double maskOverlapRatio;
  final double bboxIou;
  final double widthRatio;
  final double heightRatio;
  final double centroidAlignedOverlapPixels;
  final double centroidAlignedOverlapRatio;
  final double centroidAlignedIou;
  final double contourRawDistance;
  final double contourDifference;
  final bool usedShapeContext;
  final double huRawDistance;
  final double huDifference;
  final bool sameCreationFrame;
  final bool isDuplicate;
  final int duplicateReasonMask;

  const NodeComparison({
    required this.shapeDistance,
    required this.centroidDistance,
    required this.bboxIntersectionArea,
    required this.andOverlapPixels,
    required this.maskOverlapRatio,
    required this.bboxIou,
    required this.widthRatio,
    required this.heightRatio,
    required this.centroidAlignedOverlapPixels,
    required this.centroidAlignedOverlapRatio,
    required this.centroidAlignedIou,
    required this.contourRawDistance,
    required this.contourDifference,
    required this.usedShapeContext,
    required this.huRawDistance,
    required this.huDifference,
    required this.sameCreationFrame,
    required this.isDuplicate,
    required this.duplicateReasonMask,
  });

  List<String> get duplicateReasonLabels {
    final labels = <String>[];
    if ((duplicateReasonMask & duplicateReasonPositionalOverlap) != 0) {
      labels.add('Pos overlap');
    }
    if ((duplicateReasonMask & duplicateReasonCentroidIou) != 0) {
      labels.add('Centroid IoU');
    }
    if ((duplicateReasonMask & duplicateReasonBboxIou) != 0) {
      labels.add('BBox IoU');
    }
    if ((duplicateReasonMask & duplicateReasonShapeDifference) != 0) {
      labels.add('Shape diff');
    }
    return labels;
  }
}

class NodeOverlapAtOffset {
  final double andOverlapPixels;
  final double maskOverlapRatio;
  final double bboxIou;

  const NodeOverlapAtOffset({
    required this.andOverlapPixels,
    required this.maskOverlapRatio,
    required this.bboxIou,
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

class NodeMaskImage {
  final int width;
  final int height;
  final Uint8List rgbaBytes;

  const NodeMaskImage({
    required this.width,
    required this.height,
    required this.rgbaBytes,
  });
}
