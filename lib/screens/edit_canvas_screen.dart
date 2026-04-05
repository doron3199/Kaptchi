import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../models/graph_node_info.dart';
import '../services/native_camera_service.dart';

class EditCanvasScreen extends StatefulWidget {
  const EditCanvasScreen({super.key});

  @override
  State<EditCanvasScreen> createState() => _EditCanvasScreenState();
}

class _EditCanvasScreenState extends State<EditCanvasScreen> {
  static const double _duplicatePosOverlapThreshold = 0.50;
  static const double _duplicateCentroidIouThreshold = 0.70;
  static const double _duplicateBboxIouThreshold = 0.70;
  static const double _duplicateShapeDifferenceThreshold = 0.0001;

  final NativeCameraService _native = NativeCameraService();
  final TransformationController _transformController =
      TransformationController();

  // Snapshot taken once on open — never refreshed from C++
  List<GraphNodeInfo> _nodes = [];
  List<GraphHardEdge> _hardEdges = [];
  Rect? _canvasBounds;

  // Mask images for pixel-perfect rendering
  Map<int, ui.Image> _maskImages = {};
  bool _masksLoading = false;
  bool _darkBackground = true;
  bool _duplicateDebugMode = false;

  // Multi-select: up to two nodes
  int? _selectedIdA;
  int? _selectedIdB;
  NodeComparison? _comparison;

  // Change tracking
  final Set<int> _movedNodeIds = {};
  final Set<int> _deletedNodeIds = {};

  // Drag state
  int? _dragNodeId;
  bool _isDragging = false;
  Offset? _dragStartCanvas;
  Offset? _dragStartScene;
  Offset? _dragNodeStartCentroid;

  // Live drag overlap
  NodeOverlapAtOffset? _dragOverlap;
  int? _dragOverlapTargetId;
  DateTime _lastDragOverlapTime = DateTime(2000);

  bool get _hasChanges => _movedNodeIds.isNotEmpty || _deletedNodeIds.isNotEmpty;
  bool get _hasSelection => _selectedIdA != null || _selectedIdB != null;
  int get _duplicateNodeCount =>
      _nodes.where((node) => node.isDuplicateDebug).length;
  GraphNodeInfo? get _selectedNode {
    final id = _selectedIdB == null ? _selectedIdA : null;
    if (id == null) return null;
    return _nodes.where((node) => node.id == id).firstOrNull;
  }

  int? get _highlightedDuplicatePartnerId {
    final node = _selectedNode;
    if (node == null || !node.isDuplicateDebug || node.duplicatePartnerId < 0) {
      return null;
    }
    final partnerExists = _nodes.any((entry) => entry.id == node.duplicatePartnerId);
    return partnerExists ? node.duplicatePartnerId : null;
  }

  @override
  void initState() {
    super.initState();
    _snapshotFromCpp();
  }

  @override
  void dispose() {
    _transformController.dispose();
    super.dispose();
  }

  void _snapshotFromCpp() {
    List<GraphNodeInfo> nodes;
    try {
      nodes = _native.getGraphNodes();
    } catch (e) {
      nodes = [];
    }

    Rect? bounds;
    try {
      bounds = _native.getCanvasBounds();
    } catch (e) {
      bounds = null;
    }

    bool duplicateDebugMode;
    try {
      duplicateDebugMode = _native.getDuplicateDebugMode();
    } catch (e) {
      duplicateDebugMode = false;
    }

    List<GraphHardEdge> hardEdges;
    try {
      hardEdges = _native.getGraphHardEdges();
    } catch (e) {
      hardEdges = [];
    }

    Map<int, List<Offset>> contours;
    try {
      contours = _native.getGraphNodeContours();
    } catch (e) {
      contours = {};
    }

    final enriched = nodes
        .map((node) => GraphNodeInfo(
              id: node.id,
              bboxCanvas: node.bboxCanvas,
              centroid: node.centroid,
              area: node.area,
              absenceScore: node.absenceScore,
              lastSeenFrame: node.lastSeenFrame,
              createdFrame: node.createdFrame,
              neighborCount: node.neighborCount,
              canvasOrigin: node.canvasOrigin,
              contour: contours[node.id] ?? const [],
              isUserLocked: node.isUserLocked,
              isDuplicateDebug: node.isDuplicateDebug,
              duplicatePartnerId: node.duplicatePartnerId,
              duplicatePositionalOverlap: node.duplicatePositionalOverlap,
              duplicateCentroidIou: node.duplicateCentroidIou,
              duplicateBboxIou: node.duplicateBboxIou,
              duplicateShapeDifference: node.duplicateShapeDifference,
              duplicateReasonMask: node.duplicateReasonMask,
            ))
        .toList();

    setState(() {
      _nodes = enriched;
      _hardEdges = hardEdges;
      _canvasBounds = bounds;
      _duplicateDebugMode = duplicateDebugMode;
    });

    _loadMaskImages();
  }

  void _setDuplicateDebugMode(bool enabled) {
    _native.setDuplicateDebugMode(enabled);
    setState(() {
      _duplicateDebugMode = enabled;
    });
    _snapshotFromCpp();
  }

  void _cleanFlaggedNodes() {
    if (_duplicateNodeCount == 0) return;
    final keepDebugModeEnabled = _duplicateDebugMode;
    _native.setDuplicateDebugMode(false);
    if (keepDebugModeEnabled) {
      _native.setDuplicateDebugMode(true);
    }
    setState(() {
      _duplicateDebugMode = keepDebugModeEnabled;
      _selectedIdA = null;
      _selectedIdB = null;
      _comparison = null;
      _dragOverlap = null;
      _dragOverlapTargetId = null;
    });
    _snapshotFromCpp();
  }

  Future<void> _loadMaskImages() async {
    if (_masksLoading) return;
    _masksLoading = true;

    Map<int, NodeMaskImage> rawMasks;
    try {
      rawMasks = _native.getGraphNodeMasks();
    } catch (e) {
      debugPrint('[EditCanvas] getGraphNodeMasks failed: $e');
      _masksLoading = false;
      return;
    }

    if (rawMasks.isEmpty) {
      _masksLoading = false;
      return;
    }

    final decoded = <int, ui.Image>{};
    for (final entry in rawMasks.entries) {
      final mask = entry.value;
      if (mask.width <= 0 || mask.height <= 0) continue;
      try {
        final completer = Completer<ui.Image>();
        ui.decodeImageFromPixels(
          mask.rgbaBytes,
          mask.width,
          mask.height,
          ui.PixelFormat.rgba8888,
          completer.complete,
        );
        decoded[entry.key] = await completer.future;
      } catch (e) {
        debugPrint('[EditCanvas] decode mask ${entry.key} failed: $e');
      }
    }

    if (mounted) {
      setState(() {
        _maskImages = decoded;
      });
    }
    _masksLoading = false;
  }

  _CanvasTransform? _canvasTransformFor(Rect? bounds, Size widgetSize) {
    if (bounds == null || bounds.isEmpty) return null;
    if (widgetSize.width <= 0 || widgetSize.height <= 0) return null;
    return _CanvasTransform.fromBounds(bounds, widgetSize);
  }

  Offset _screenToCanvas(Offset screenPoint, Size widgetSize) {
    final transform = _canvasTransformFor(_canvasBounds, widgetSize);
    if (transform == null) return Offset.zero;
    return transform.screenToCanvas(screenPoint);
  }

  int? _findNodeAtPosition(Offset localPos, Size widgetSize) {
    final transform = _canvasTransformFor(_canvasBounds, widgetSize);
    if (transform == null) return null;

    const hitTolerance = 10.0;
    double bestScore = double.infinity;
    int? bestId;

    for (final node in _nodes) {
      final score =
          _nodeHitScore(node, localPos, transform, tolerance: hitTolerance);
      if (score == null) continue;
      if (score < bestScore) {
        bestScore = score;
        bestId = node.id;
      }
    }
    return bestId;
  }

  double? _nodeHitScore(
    GraphNodeInfo node,
    Offset screenPoint,
    _CanvasTransform transform, {
    required double tolerance,
  }) {
    final screenBbox = transform.canvasRectToScreen(node.bboxCanvas);
    final expandedBbox = screenBbox.inflate(tolerance);
    if (!expandedBbox.contains(screenPoint)) return null;

    final screenContour = [
      for (final point in node.contour) transform.canvasToScreen(point),
    ];

    if (screenContour.length >= 3) {
      final path = ui.Path()
        ..moveTo(screenContour.first.dx, screenContour.first.dy);
      for (int i = 1; i < screenContour.length; i++) {
        path.lineTo(screenContour[i].dx, screenContour[i].dy);
      }
      path.close();

      if (path.contains(screenPoint)) return 0.0;

      final edgeDistance =
          _distanceToClosedPolyline(screenPoint, screenContour);
      if (edgeDistance <= tolerance) return edgeDistance;
    }

    if (screenBbox.contains(screenPoint)) return 0.5;

    final centroidDistance =
        (transform.canvasToScreen(node.centroid) - screenPoint).distance;
    if (centroidDistance <= tolerance * 1.5) return centroidDistance + 1.0;

    return null;
  }

  double _distanceToClosedPolyline(Offset point, List<Offset> polyline) {
    if (polyline.length < 2) return double.infinity;
    double best = double.infinity;
    for (int i = 0; i < polyline.length; i++) {
      final a = polyline[i];
      final b = polyline[(i + 1) % polyline.length];
      best = math.min(best, _distanceToSegment(point, a, b));
    }
    return best;
  }

  double _distanceToSegment(Offset p, Offset a, Offset b) {
    final ab = b - a;
    final ap = p - a;
    final abLenSquared = ab.dx * ab.dx + ab.dy * ab.dy;
    if (abLenSquared <= 0.0001) return (p - a).distance;
    final t = ((ap.dx * ab.dx) + (ap.dy * ab.dy)) / abLenSquared;
    final clampedT = t.clamp(0.0, 1.0);
    final projection = Offset(
      a.dx + ab.dx * clampedT,
      a.dy + ab.dy * clampedT,
    );
    return (p - projection).distance;
  }

  void _resetDragState() {
    if (!mounted) return;
    setState(() {
      _dragNodeId = null;
      _isDragging = false;
      _dragStartCanvas = null;
      _dragStartScene = null;
      _dragNodeStartCentroid = null;
      _dragOverlap = null;
      _dragOverlapTargetId = null;
    });
  }

  void _onTapNode(int nodeId) {
    setState(() {
      if (nodeId == _selectedIdA) {
        _selectedIdA = _selectedIdB;
        _selectedIdB = null;
        _comparison = null;
      } else if (nodeId == _selectedIdB) {
        _selectedIdB = null;
        _comparison = null;
      } else if (_selectedIdA == null) {
        _selectedIdA = nodeId;
        _comparison = null;
      } else if (_selectedIdB == null) {
        _selectedIdB = nodeId;
        _updateComparison();
      } else {
        _selectedIdA = nodeId;
        _selectedIdB = null;
        _comparison = null;
      }
    });
  }

  void _updateComparison() {
    if (_selectedIdA != null && _selectedIdB != null) {
      _comparison = _native.compareNodes(_selectedIdA!, _selectedIdB!);
    } else {
      _comparison = null;
    }
  }

  void _onDragEnd(int nodeId) {
    _movedNodeIds.add(nodeId);
    // Recompute comparison if dragged node is one of the selected pair
    if (nodeId == _selectedIdA || nodeId == _selectedIdB) {
      _updateComparison();
    }
  }

  void _updateDragOverlap(int draggedNodeId, Offset canvasDelta) {
    final now = DateTime.now();
    if (now.difference(_lastDragOverlapTime).inMilliseconds < 100) return;
    _lastDragOverlapTime = now;

    final draggedNode = _nodes.where((n) => n.id == draggedNodeId).firstOrNull;
    if (draggedNode == null) return;

    int? bestTargetId;
    double bestDist = 80.0; // max centroid distance to consider
    for (final node in _nodes) {
      if (node.id == draggedNodeId) continue;
      final dist = (node.centroid - draggedNode.centroid).distance;
      if (dist < bestDist) {
        bestDist = dist;
        bestTargetId = node.id;
      }
    }

    if (bestTargetId != null) {
      final overlap = _native.compareNodesAtOffset(
        draggedNodeId, bestTargetId, canvasDelta.dx, canvasDelta.dy);
      setState(() {
        _dragOverlap = overlap;
        _dragOverlapTargetId = bestTargetId;
      });
    } else {
      setState(() {
        _dragOverlap = null;
        _dragOverlapTargetId = null;
      });
    }
  }

  void _deleteSelected() {
    final id = _selectedIdB ?? _selectedIdA;
    if (id == null) return;
    setState(() {
      _nodes.removeWhere((n) => n.id == id);
      _deletedNodeIds.add(id);
      _movedNodeIds.remove(id);
      if (id == _selectedIdB) {
        _selectedIdB = null;
      } else {
        _selectedIdA = _selectedIdB;
        _selectedIdB = null;
      }
      _comparison = null;
    });
  }

  void _saveChanges() {
    final moves = <({int id, double cx, double cy})>[];
    for (final id in _movedNodeIds) {
      final node = _nodes.where((n) => n.id == id).firstOrNull;
      if (node == null) continue;
      moves.add((id: id, cx: node.centroid.dx, cy: node.centroid.dy));
    }
    debugPrint('[EditCanvas] saving: '
        'deletes=${_deletedNodeIds.toList()}, '
        'moves=${moves.map((m) => "(${m.id},${m.cx.toStringAsFixed(1)},${m.cy.toStringAsFixed(1)})").toList()}');
    final result = _native.applyUserEdits(
      deleteIds: _deletedNodeIds.toList(),
      moves: moves,
    );
    debugPrint('[EditCanvas] applyUserEdits returned: $result');
    Navigator.pop(context);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.grey[900],
        title: Text(
          'Edit Canvas (${_nodes.length} nodes, $_duplicateNodeCount flagged)',
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh, color: Colors.white70),
            tooltip: 'Refresh graph snapshot',
            onPressed: _snapshotFromCpp,
          ),
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(
                'Dup debug',
                style: TextStyle(
                  color: _duplicateDebugMode
                      ? Colors.redAccent
                      : Colors.white70,
                  fontSize: 12,
                  fontWeight: FontWeight.w600,
                ),
              ),
              Switch.adaptive(
                value: _duplicateDebugMode,
                activeThumbColor: Colors.redAccent,
                activeTrackColor: const Color(0x66FF1744),
                onChanged: _setDuplicateDebugMode,
              ),
            ],
          ),
          IconButton(
            icon: Icon(
              _darkBackground ? Icons.light_mode : Icons.dark_mode,
              color: Colors.white70,
            ),
            tooltip: 'Toggle background',
            onPressed: () => setState(() => _darkBackground = !_darkBackground),
          ),
          if (_hasSelection)
            IconButton(
              icon: const Icon(Icons.delete, color: Colors.redAccent),
              tooltip: 'Delete selected node',
              onPressed: _deleteSelected,
            ),
          if (_hasChanges)
            IconButton(
              icon: const Icon(Icons.save, color: Colors.greenAccent),
              tooltip: 'Save changes',
              onPressed: _saveChanges,
            ),
        ],
      ),
      body: _buildCanvasView(),
    );
  }

  Widget _buildCanvasView() {
    if (_canvasBounds == null ||
        _canvasBounds!.isEmpty ||
        _nodes.isEmpty) {
      return const Center(
        child: Text(
          'No canvas content yet.\nCapture some whiteboard strokes first.',
          textAlign: TextAlign.center,
          style: TextStyle(color: Colors.grey, fontSize: 16),
        ),
      );
    }

    return LayoutBuilder(
      builder: (context, constraints) {
        final widgetSize = Size(constraints.maxWidth, constraints.maxHeight);
        return Stack(
          children: [
            Listener(
              behavior: HitTestBehavior.opaque,
              onPointerDown: (event) {
                final scenePoint =
                    _transformController.toScene(event.localPosition);
                final nodeId = _findNodeAtPosition(scenePoint, widgetSize);
                if (nodeId == null) return;

                final node = _nodes.where((n) => n.id == nodeId).firstOrNull;
                if (node == null) return;

                setState(() {
                  _dragNodeId = nodeId;
                  _isDragging = false;
                  _dragStartScene = scenePoint;
                  _dragStartCanvas = _screenToCanvas(scenePoint, widgetSize);
                  _dragNodeStartCentroid = node.centroid;
                });
              },
              onPointerMove: (event) {
                if (_dragNodeId == null ||
                    _dragStartCanvas == null ||
                    _dragStartScene == null ||
                    _dragNodeStartCentroid == null) {
                  return;
                }

                final scenePoint =
                    _transformController.toScene(event.localPosition);
                final sceneDelta = scenePoint - _dragStartScene!;
                final canvasPoint = _screenToCanvas(scenePoint, widgetSize);
                final canvasDelta = canvasPoint - _dragStartCanvas!;

                if (!_isDragging && sceneDelta.distance < 4.0) return;

                setState(() {
                  _isDragging = true;
                  final idx = _nodes.indexWhere((n) => n.id == _dragNodeId);
                  if (idx >= 0) {
                    _nodes[idx] = _nodes[idx].copyWithPosition(
                      newCentroid: _dragNodeStartCentroid! + canvasDelta,
                    );
                  }
                });

                _updateDragOverlap(_dragNodeId!, canvasDelta);
              },
              onPointerUp: (_) {
                final tappedNodeId = _dragNodeId;
                final wasDragging = _isDragging;
                if (wasDragging && tappedNodeId != null) {
                  _onDragEnd(tappedNodeId);
                }
                _resetDragState();
                if (!wasDragging && tappedNodeId != null) {
                  _onTapNode(tappedNodeId);
                }
              },
              onPointerCancel: (_) => _resetDragState(),
              child: InteractiveViewer(
                transformationController: _transformController,
                boundaryMargin: const EdgeInsets.all(200),
                minScale: 0.1,
                maxScale: 10.0,
                panEnabled: _dragNodeId == null,
                scaleEnabled: _dragNodeId == null,
                child: SizedBox(
                  width: widgetSize.width,
                  height: widgetSize.height,
                  child: CustomPaint(
                    painter: _EditCanvasPainter(
                      nodes: _nodes,
                      hardEdges: _hardEdges,
                      canvasBounds: _canvasBounds,
                      selectedIdA: _selectedIdA,
                      selectedIdB: _selectedIdB,
                      highlightedDuplicatePartnerId:
                          _highlightedDuplicatePartnerId,
                      movedIds: _movedNodeIds,
                      dragOverlapTargetId: _dragOverlapTargetId,
                      maskImages: _maskImages,
                      darkBackground: _darkBackground,
                    ),
                    size: widgetSize,
                  ),
                ),
              ),
            ),
            Positioned(
              top: 8,
              left: 8,
              child: _buildStatusCard(),
            ),
            // Drag overlap floating label
            if (_isDragging && _dragOverlap != null && _dragOverlapTargetId != null)
              Positioned(
                top: 8,
                right: 8,
                child: _buildDragOverlapLabel(),
              ),
            // Comparison info panel
            if (_comparison != null)
              Positioned(
                bottom: 0,
                left: 0,
                right: 0,
                child: _buildComparisonPanel(),
              ),
            if (_comparison == null && _selectedNode != null)
              Positioned(
                bottom: 0,
                left: 0,
                right: 0,
                child: _buildSelectedNodePanel(_selectedNode!),
              ),
          ],
        );
      },
    );
  }

  Widget _buildStatusCard() {
    return Container(
      constraints: const BoxConstraints(maxWidth: 320),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      decoration: BoxDecoration(
        color: const Color(0xDD101010),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(
          color: _duplicateDebugMode ? Colors.redAccent : Colors.white24,
        ),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            _duplicateDebugMode
                ? 'Duplicate debug mode ON'
                : 'Duplicate debug mode OFF',
            style: TextStyle(
              color: _duplicateDebugMode ? Colors.redAccent : Colors.white,
              fontWeight: FontWeight.bold,
              fontSize: 12,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            _duplicateDebugMode
                ? 'Would-be deleted duplicates stay hidden from the main canvas and appear only in this edit view.'
                : 'Normal duplicate deletion is active.',
            style: const TextStyle(color: Colors.white70, fontSize: 11),
          ),
          const SizedBox(height: 6),
          Text(
            'Flagged nodes: $_duplicateNodeCount',
            style: const TextStyle(color: Colors.white, fontSize: 11),
          ),
          Text(
            'Selected: ${_selectedIdA ?? '-'} / ${_selectedIdB ?? '-'}',
            style: const TextStyle(color: Colors.white54, fontSize: 11),
          ),
          if (_duplicateNodeCount > 0) ...[
            const SizedBox(height: 8),
            SizedBox(
              width: double.infinity,
              child: TextButton.icon(
                onPressed: _cleanFlaggedNodes,
                icon: const Icon(Icons.cleaning_services, size: 16),
                label: const Text('Clean flagged nodes'),
                style: TextButton.styleFrom(
                  foregroundColor: Colors.redAccent,
                  side: const BorderSide(color: Color(0x66FF1744)),
                ),
              ),
            ),
          ],
        ],
      ),
    );
  }

  Widget _buildDragOverlapLabel() {
    final overlap = _dragOverlap!;
    final ratioColor = overlap.maskOverlapRatio > 0.60
        ? Colors.greenAccent
        : overlap.maskOverlapRatio > 0.15
            ? Colors.yellowAccent
            : Colors.redAccent;
    return Container(
      padding: const EdgeInsets.all(8),
      decoration: BoxDecoration(
        color: Colors.black87,
        borderRadius: BorderRadius.circular(6),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.end,
        mainAxisSize: MainAxisSize.min,
        children: [
          Text('Target: #$_dragOverlapTargetId',
              style: const TextStyle(color: Colors.white70, fontSize: 11)),
          Text(
            'Mask overlap: ${(overlap.maskOverlapRatio * 100).toStringAsFixed(1)}%',
            style: TextStyle(color: ratioColor, fontSize: 12, fontWeight: FontWeight.bold),
          ),
          Text(
            'BBox IoU: ${(overlap.bboxIou * 100).toStringAsFixed(1)}%',
            style: const TextStyle(color: Colors.white70, fontSize: 11),
          ),
        ],
      ),
    );
  }

  Widget _buildComparisonPanel() {
    final comp = _comparison!;
    final reasonText = comp.duplicateReasonLabels.isEmpty
      ? 'None'
      : comp.duplicateReasonLabels.join(', ');
    final contourLabel = comp.usedShapeContext
        ? 'Contour (Shape Context)'
        : 'Contour (matchShapes)';
    final positionalGate = comp.maskOverlapRatio > _duplicatePosOverlapThreshold;
    final centroidIouGate = comp.centroidAlignedIou > _duplicateCentroidIouThreshold;
    final bboxIouGate = comp.bboxIou > _duplicateBboxIouThreshold;
    final shapeGate = !comp.sameCreationFrame &&
      comp.shapeDistance < _duplicateShapeDifferenceThreshold;
    return Container(
      constraints: const BoxConstraints(maxHeight: 220),
      color: const Color(0xDD1A1A1A),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(
              children: [
                Text(
                  'Compare #$_selectedIdA vs #$_selectedIdB',
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 13,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const Spacer(),
                InkWell(
                  onTap: () => setState(() {
                    _selectedIdB = null;
                    _comparison = null;
                  }),
                  child: const Icon(Icons.close, color: Colors.white54, size: 18),
                ),
              ],
            ),
            const SizedBox(height: 4),
            _metricRow(
              'Native duplicate verdict',
              comp.isDuplicate ? 'YES' : 'NO',
              comp.isDuplicate ? Colors.greenAccent : Colors.redAccent,
            ),
            _metricRow(
              'Triggered reasons',
              reasonText,
              comp.isDuplicate ? Colors.greenAccent : Colors.white70,
            ),
            _metricRow(
              'Same creation frame',
              comp.sameCreationFrame ? 'YES' : 'NO',
              comp.sameCreationFrame ? Colors.yellowAccent : Colors.white70,
            ),
            _metricRow('Centroid dist', '${comp.centroidDistance.toStringAsFixed(1)} px',
                _threshColor(comp.centroidDistance, 40, 80, lowerIsBetter: true)),
            _metricRow('BBox IoU', '${(comp.bboxIou * 100).toStringAsFixed(1)}%',
                _threshColor(comp.bboxIou, 0.50, 0.20, lowerIsBetter: false)),
            _metricRow('Width ratio', '${(comp.widthRatio * 100).toStringAsFixed(1)}%',
                _threshColor(comp.widthRatio, 0.70, 0.50, lowerIsBetter: false)),
            _metricRow('Height ratio', '${(comp.heightRatio * 100).toStringAsFixed(1)}%',
                _threshColor(comp.heightRatio, 0.70, 0.50, lowerIsBetter: false)),
            _metricRow(
              'Pos overlap gate (> 50%)',
              '${(comp.maskOverlapRatio * 100).toStringAsFixed(1)}% -> ${positionalGate ? 'YES' : 'NO'}',
              positionalGate ? Colors.greenAccent : Colors.redAccent,
            ),
            _metricRow(
              'Centroid IoU gate (> 70%)',
              '${(comp.centroidAlignedIou * 100).toStringAsFixed(1)}% -> ${centroidIouGate ? 'YES' : 'NO'}',
              centroidIouGate ? Colors.greenAccent : Colors.redAccent,
            ),
            _metricRow(
              'BBox IoU gate (> 70%)',
              '${(comp.bboxIou * 100).toStringAsFixed(1)}% -> ${bboxIouGate ? 'YES' : 'NO'}',
              bboxIouGate ? Colors.greenAccent : Colors.redAccent,
            ),
            _metricRow(
              'Shape diff gate (< 0.0001)',
              comp.sameCreationFrame
                  ? '${comp.shapeDistance.toStringAsFixed(4)} -> BLOCKED same frame'
                  : '${comp.shapeDistance.toStringAsFixed(4)} -> ${shapeGate ? 'YES' : 'NO'}',
              comp.sameCreationFrame
                  ? Colors.yellowAccent
                  : shapeGate
                      ? Colors.greenAccent
                      : Colors.redAccent,
            ),
            _metricRow('Total shape diff', comp.shapeDistance.toStringAsFixed(4),
                _threshColor(comp.shapeDistance, 0.20, 0.45, lowerIsBetter: true)),
            _metricRow(
              contourLabel,
              'raw ${comp.contourRawDistance.toStringAsFixed(4)} | diff ${comp.contourDifference.toStringAsFixed(4)}',
              _threshColor(comp.contourDifference, 0.20, 0.45, lowerIsBetter: true),
            ),
            _metricRow(
              'Hu moments',
              'raw ${comp.huRawDistance.toStringAsFixed(4)} | diff ${comp.huDifference.toStringAsFixed(4)}',
              _threshColor(comp.huDifference, 0.20, 0.45, lowerIsBetter: true),
            ),
            _metricRow('AND pixels', '${comp.andOverlapPixels.toInt()} px', Colors.white70),
            _metricRow('AND pixels (centroid)', '${comp.centroidAlignedOverlapPixels.toInt()} px', Colors.white70),
            _metricRow('BBox intersect', '${comp.bboxIntersectionArea.toInt()} px\u00B2', Colors.white70),
          ],
        ),
      ),
    );
  }

  Widget _buildSelectedNodePanel(GraphNodeInfo node) {
    final reasonText = node.duplicateReasonLabels.isEmpty
        ? 'None'
        : node.duplicateReasonLabels.join(', ');
    return Container(
      constraints: const BoxConstraints(maxHeight: 220),
      color: const Color(0xDD1A1A1A),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      child: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(
              children: [
                Text(
                  'Node #${node.id}',
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 13,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(width: 8),
                if (node.isDuplicateDebug)
                  Container(
                    padding:
                        const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                    decoration: BoxDecoration(
                      color: const Color(0x33FF1744),
                      borderRadius: BorderRadius.circular(999),
                      border: Border.all(color: const Color(0xFFFF1744)),
                    ),
                    child: const Text(
                      'FLAGGED DUPLICATE',
                      style: TextStyle(
                        color: Color(0xFFFF1744),
                        fontSize: 10,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ),
                const Spacer(),
                InkWell(
                  onTap: () => setState(() {
                    _selectedIdA = null;
                    _selectedIdB = null;
                    _comparison = null;
                  }),
                  child: const Icon(Icons.close, color: Colors.white54, size: 18),
                ),
              ],
            ),
            const SizedBox(height: 4),
            _metricRow('Area', '${node.area.toStringAsFixed(1)} px²', Colors.white70),
            _metricRow('Absence score', node.absenceScore.toStringAsFixed(2), Colors.white70),
            _metricRow('Created frame', '${node.createdFrame}', Colors.white70),
            _metricRow('Last seen frame', '${node.lastSeenFrame}', Colors.white70),
            _metricRow('Hard-edge degree', '${node.neighborCount}', Colors.white70),
            _metricRow('User locked', node.isUserLocked ? 'YES' : 'NO',
                node.isUserLocked ? Colors.greenAccent : Colors.white70),
            _metricRow('Duplicate debug', node.isDuplicateDebug ? 'ON' : 'OFF',
                node.isDuplicateDebug ? Colors.redAccent : Colors.white70),
            if (node.isDuplicateDebug) ...[
              _metricRow('Partner node', '#${node.duplicatePartnerId}', Colors.redAccent),
              _metricRow('Reasons', reasonText, Colors.redAccent),
              _metricRow(
                'Pos overlap',
                '${(node.duplicatePositionalOverlap * 100).toStringAsFixed(1)}%',
                _threshColor(
                  node.duplicatePositionalOverlap,
                  _duplicatePosOverlapThreshold,
                  _duplicatePosOverlapThreshold * 0.5,
                  lowerIsBetter: false,
                ),
              ),
              _metricRow(
                'Centroid IoU',
                '${(node.duplicateCentroidIou * 100).toStringAsFixed(1)}%',
                _threshColor(node.duplicateCentroidIou, _duplicateCentroidIouThreshold, 0.35,
                    lowerIsBetter: false),
              ),
              _metricRow(
                'BBox IoU',
                '${(node.duplicateBboxIou * 100).toStringAsFixed(1)}%',
                _threshColor(node.duplicateBboxIou, _duplicateBboxIouThreshold, 0.35,
                    lowerIsBetter: false),
              ),
              _metricRow(
                'Shape diff',
                node.duplicateShapeDifference.toStringAsFixed(4),
                _threshColor(node.duplicateShapeDifference, _duplicateShapeDifferenceThreshold, 0.45,
                    lowerIsBetter: true),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Color _threshColor(double value, double goodThresh, double badThresh,
      {required bool lowerIsBetter}) {
    if (lowerIsBetter) {
      if (value <= goodThresh) return Colors.greenAccent;
      if (value >= badThresh) return Colors.redAccent;
      return Colors.yellowAccent;
    } else {
      if (value >= goodThresh) return Colors.greenAccent;
      if (value <= badThresh) return Colors.redAccent;
      return Colors.yellowAccent;
    }
  }

  Widget _metricRow(String label, String value, Color valueColor) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 1),
      child: Row(
        children: [
          SizedBox(
            width: 160,
            child: Text(label,
                style: const TextStyle(color: Colors.white54, fontSize: 11)),
          ),
          Text(value,
              style: TextStyle(
                  color: valueColor,
                  fontSize: 11,
                  fontWeight: FontWeight.w600)),
        ],
      ),
    );
  }
}

class _EditCanvasPainter extends CustomPainter {
  _EditCanvasPainter({
    required this.nodes,
    required this.hardEdges,
    required this.canvasBounds,
    required this.selectedIdA,
    required this.selectedIdB,
    this.highlightedDuplicatePartnerId,
    required this.movedIds,
    this.dragOverlapTargetId,
    this.maskImages = const {},
    this.darkBackground = true,
  });

  final List<GraphNodeInfo> nodes;
  final List<GraphHardEdge> hardEdges;
  final Rect? canvasBounds;
  final int? selectedIdA;
  final int? selectedIdB;
  final int? highlightedDuplicatePartnerId;
  final Set<int> movedIds;
  final int? dragOverlapTargetId;
  final Map<int, ui.Image> maskImages;
  final bool darkBackground;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = darkBackground ? const Color(0xFF1A1A1A) : const Color(0xFFFFFFFF),
    );

    if (canvasBounds == null || canvasBounds!.isEmpty || nodes.isEmpty) return;

    final transform = _CanvasTransform.fromBounds(canvasBounds!, size);
    final nodeById = {for (final node in nodes) node.id: node};

    final hardEdgePaint = Paint()
      ..color = darkBackground
          ? const Color(0x6696F0FF)
          : const Color(0x88427B8A)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final selectedHardEdgePaint = Paint()
      ..color = const Color(0xFF00E5FF)
      ..strokeWidth = 2.5
      ..style = PaintingStyle.stroke;

    final selectedStrokeA = Paint()
      ..color = const Color(0xA04488FF)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final selectedStrokeB = Paint()
      ..color = const Color(0xA0FF8844)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final movedStroke = Paint()
      ..color = const Color(0x8088FF88)
      ..strokeWidth = 1.0
      ..style = PaintingStyle.stroke;

    final overlapTargetStroke = Paint()
      ..color = const Color(0xA0FF44FF)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final duplicateStroke = Paint()
      ..color = const Color(0xA0FF1744)
      ..strokeWidth = 1.0
      ..style = PaintingStyle.stroke;

    final duplicatePartnerStroke = Paint()
      ..color = const Color(0xA0FFFF00)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final centroidPaint = Paint()
      ..color = const Color(0x80FF0000)
      ..style = PaintingStyle.fill;

    final centroidSelectedPaint = Paint()
      ..color = const Color(0x80FF4444)
      ..style = PaintingStyle.fill;

    final textPainter = TextPainter(textDirection: TextDirection.ltr);

    for (final edge in hardEdges) {
      final first = nodeById[edge.firstNodeId];
      final second = nodeById[edge.secondNodeId];
      if (first == null || second == null) continue;

      final linePaint = first.id == selectedIdA ||
              first.id == selectedIdB ||
              second.id == selectedIdA ||
              second.id == selectedIdB
          ? selectedHardEdgePaint
          : hardEdgePaint;

      canvas.drawLine(
        transform.canvasToScreen(first.centroid),
        transform.canvasToScreen(second.centroid),
        linePaint,
      );
    }

    for (final node in nodes) {
      final screenCentroid = transform.canvasToScreen(node.centroid);
      final screenBbox = transform.canvasRectToScreen(node.bboxCanvas);
      final isSelectedA = node.id == selectedIdA;
      final isSelectedB = node.id == selectedIdB;
      final isSelected = isSelectedA || isSelectedB;
      final isMoved = movedIds.contains(node.id);
      final isOverlapTarget = node.id == dragOverlapTargetId;
      final isDuplicateDebug = node.isDuplicateDebug;
      final isDuplicatePartner = node.id == highlightedDuplicatePartnerId;

      // Render color image (enhanced color_pixels from C++)
      final maskImage = maskImages[node.id];
      if (maskImage != null) {
        final src = Rect.fromLTWH(
          0, 0,
          maskImage.width.toDouble(),
          maskImage.height.toDouble(),
        );
        canvas.drawImageRect(maskImage, src, screenBbox, Paint());
      }

      if (isDuplicateDebug) {
        canvas.drawRect(screenBbox, duplicateStroke);
      }

      if (isDuplicatePartner) {
        canvas.drawRect(screenBbox, duplicatePartnerStroke);
      }

      // Selection / overlap / moved stroke overlays
      if (isSelectedA || isSelectedB || isOverlapTarget || isMoved) {
        final strokePaint = isSelectedA
            ? selectedStrokeA
            : isSelectedB
                ? selectedStrokeB
                : isOverlapTarget
                    ? overlapTargetStroke
                    : movedStroke;
        canvas.drawRect(screenBbox, strokePaint);
      }

      canvas.drawCircle(
        screenCentroid,
        isSelected ? 2.25 : 1.5,
        isSelected ? centroidSelectedPaint : centroidPaint,
      );

      textPainter.text = TextSpan(
        text: '${node.id}',
        style: TextStyle(
          color: isSelectedA
              ? const Color(0xFF4488FF)
              : isSelectedB
                  ? const Color(0xFFFF8844)
                : isDuplicateDebug
                  ? const Color(0xFFFF1744)
                  : darkBackground
                      ? const Color(0xFFFFFF00)
                      : const Color(0xFFCC8800),
          fontSize: isSelected ? 12 : 10,
          fontWeight: FontWeight.bold,
        ),
      );
      textPainter.layout();
      textPainter.paint(canvas, screenCentroid + const Offset(5, -12));

      if (isDuplicateDebug && node.duplicatePartnerId >= 0) {
        textPainter.text = const TextSpan();
        textPainter.text = TextSpan(
          text: 'vs #${node.duplicatePartnerId}',
          style: const TextStyle(
            color: Color(0xFFFFFF00),
            fontSize: 9,
            fontWeight: FontWeight.w700,
          ),
        );
        textPainter.layout();
        textPainter.paint(canvas, screenCentroid + const Offset(5, 2));
      }
    }
  }

  @override
  bool shouldRepaint(_EditCanvasPainter oldDelegate) {
    return oldDelegate.nodes != nodes ||
      oldDelegate.hardEdges != hardEdges ||
        oldDelegate.canvasBounds != canvasBounds ||
        oldDelegate.selectedIdA != selectedIdA ||
        oldDelegate.selectedIdB != selectedIdB ||
        oldDelegate.highlightedDuplicatePartnerId !=
          highlightedDuplicatePartnerId ||
        oldDelegate.movedIds != movedIds ||
        oldDelegate.dragOverlapTargetId != dragOverlapTargetId ||
        oldDelegate.maskImages != maskImages ||
        oldDelegate.darkBackground != darkBackground;
  }
}

class _CanvasTransform {
  const _CanvasTransform({
    required this.bounds,
    required this.scale,
    required this.contentOffset,
  });

  factory _CanvasTransform.fromBounds(Rect bounds, Size viewportSize) {
    final scale = math.min(
      viewportSize.width / bounds.width,
      viewportSize.height / bounds.height,
    );
    final contentSize = Size(bounds.width * scale, bounds.height * scale);
    final contentOffset = Offset(
      (viewportSize.width - contentSize.width) / 2,
      (viewportSize.height - contentSize.height) / 2,
    );
    return _CanvasTransform(
      bounds: bounds,
      scale: scale,
      contentOffset: contentOffset,
    );
  }

  final Rect bounds;
  final double scale;
  final Offset contentOffset;

  Offset canvasToScreen(Offset canvasPoint) {
    return Offset(
      contentOffset.dx + (canvasPoint.dx - bounds.left) * scale,
      contentOffset.dy + (canvasPoint.dy - bounds.top) * scale,
    );
  }

  Offset screenToCanvas(Offset screenPoint) {
    return Offset(
      (screenPoint.dx - contentOffset.dx) / scale + bounds.left,
      (screenPoint.dy - contentOffset.dy) / scale + bounds.top,
    );
  }

  Rect canvasRectToScreen(Rect canvasRect) {
    final topLeft = canvasToScreen(canvasRect.topLeft);
    return Rect.fromLTWH(
      topLeft.dx,
      topLeft.dy,
      canvasRect.width * scale,
      canvasRect.height * scale,
    );
  }
}
