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
  final NativeCameraService _native = NativeCameraService();
  final TransformationController _transformController =
      TransformationController();

  // Snapshot taken once on open — never refreshed from C++
  List<GraphNodeInfo> _nodes = [];
  Rect? _canvasBounds;
  int? _selectedNodeId;

  // Change tracking
  final Set<int> _movedNodeIds = {};
  final Set<int> _deletedNodeIds = {};

  // Drag state
  int? _dragNodeId;
  bool _isDragging = false;
  Offset? _dragStartCanvas;
  Offset? _dragStartScene;
  Offset? _dragNodeStartCentroid;

  bool get _hasChanges => _movedNodeIds.isNotEmpty || _deletedNodeIds.isNotEmpty;

  @override
  void initState() {
    super.initState();
    // Lock all nodes so the pipeline can't remove/modify them while editing
    final locked = _native.lockAllGraphNodes();
    debugPrint('[EditCanvas] locked $locked nodes');
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
            ))
        .toList();

    setState(() {
      _nodes = enriched;
      _canvasBounds = bounds;
    });
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
    });
  }

  void _onTapNode(int nodeId) {
    setState(() {
      _selectedNodeId = _selectedNodeId == nodeId ? null : nodeId;
    });
  }

  void _onDragEnd(int nodeId) {
    // Just record that this node was moved — actual C++ call happens on Save
    _movedNodeIds.add(nodeId);
  }

  void _deleteSelected() {
    final id = _selectedNodeId;
    if (id == null) return;
    setState(() {
      _nodes.removeWhere((n) => n.id == id);
      _selectedNodeId = null;
      _deletedNodeIds.add(id);
      _movedNodeIds.remove(id); // no need to move a deleted node
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
        title: Text('Edit Canvas (${_nodes.length} nodes)'),
        actions: [
          if (_selectedNodeId != null)
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
        return Listener(
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
                  canvasBounds: _canvasBounds,
                  selectedId: _selectedNodeId,
                  movedIds: _movedNodeIds,
                ),
                size: widgetSize,
              ),
            ),
          ),
        );
      },
    );
  }
}

class _EditCanvasPainter extends CustomPainter {
  _EditCanvasPainter({
    required this.nodes,
    required this.canvasBounds,
    required this.selectedId,
    required this.movedIds,
  });

  final List<GraphNodeInfo> nodes;
  final Rect? canvasBounds;
  final int? selectedId;
  final Set<int> movedIds;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = const Color(0xFF1A1A1A),
    );

    if (canvasBounds == null || canvasBounds!.isEmpty || nodes.isEmpty) return;

    final transform = _CanvasTransform.fromBounds(canvasBounds!, size);

    final normalFill = Paint()
      ..color = const Color(0xDDFFFFFF)
      ..style = PaintingStyle.fill;

    final movedFill = Paint()
      ..color = const Color(0xDDE0FFE0)
      ..style = PaintingStyle.fill;

    final normalStroke = Paint()
      ..color = const Color(0x88FFFFFF)
      ..strokeWidth = 1.0
      ..style = PaintingStyle.stroke;

    final selectedStroke = Paint()
      ..color = const Color(0xFF4488FF)
      ..strokeWidth = 3.0
      ..style = PaintingStyle.stroke;

    final movedStroke = Paint()
      ..color = const Color(0xFF88FF88)
      ..strokeWidth = 1.5
      ..style = PaintingStyle.stroke;

    final centroidPaint = Paint()
      ..color = const Color(0xFFFF0000)
      ..style = PaintingStyle.fill;

    final textPainter = TextPainter(textDirection: TextDirection.ltr);

    for (final node in nodes) {
      final screenCentroid = transform.canvasToScreen(node.centroid);
      final isSelected = node.id == selectedId;
      final isMoved = movedIds.contains(node.id);
      final fill = isMoved ? movedFill : normalFill;

      if (node.contour.length >= 3) {
        final path = ui.Path();
        final first = transform.canvasToScreen(node.contour[0]);
        path.moveTo(first.dx, first.dy);
        for (int i = 1; i < node.contour.length; i++) {
          final pt = transform.canvasToScreen(node.contour[i]);
          path.lineTo(pt.dx, pt.dy);
        }
        path.close();
        canvas.drawPath(path, fill);

        if (isSelected) {
          canvas.drawPath(path, selectedStroke);
        } else if (isMoved) {
          canvas.drawPath(path, movedStroke);
        }
      } else {
        final screenBbox = transform.canvasRectToScreen(node.bboxCanvas);
        canvas.drawRect(screenBbox, normalStroke);
        if (isSelected) {
          canvas.drawRect(screenBbox, selectedStroke);
        }
      }

      canvas.drawCircle(screenCentroid, 3.0, centroidPaint);

      textPainter.text = TextSpan(
        text: '${node.id}',
        style: const TextStyle(
          color: Color(0xFFFFFF00),
          fontSize: 10,
          fontWeight: FontWeight.bold,
        ),
      );
      textPainter.layout();
      textPainter.paint(canvas, screenCentroid + const Offset(5, -12));
    }
  }

  @override
  bool shouldRepaint(_EditCanvasPainter oldDelegate) {
    return oldDelegate.nodes != nodes ||
        oldDelegate.canvasBounds != canvasBounds ||
        oldDelegate.selectedId != selectedId ||
        oldDelegate.movedIds != movedIds;
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
