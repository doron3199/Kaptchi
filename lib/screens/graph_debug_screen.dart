import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../models/graph_node_info.dart';
import '../services/app_logger.dart';
import '../services/native_camera_service.dart';

class GraphDebugScreen extends StatefulWidget {
  const GraphDebugScreen({super.key});

  @override
  State<GraphDebugScreen> createState() => _GraphDebugScreenState();
}

enum _GraphDebugMode { live, compare }

class _SnapshotGraphState {
  const _SnapshotGraphState({
    required this.slot,
    this.nodes = const [],
    this.canvasBounds,
    this.selectedId,
    this.contours = const {},
    this.statusMessage,
  });

  final int slot;
  final List<GraphNodeInfo> nodes;
  final Rect? canvasBounds;
  final int? selectedId;
  final Map<int, List<Offset>> contours;
  final String? statusMessage;

  _SnapshotGraphState copyWith({
    List<GraphNodeInfo>? nodes,
    Rect? canvasBounds,
    bool clearCanvasBounds = false,
    int? selectedId,
    bool clearSelectedId = false,
    Map<int, List<Offset>>? contours,
    String? statusMessage,
    bool clearStatusMessage = false,
  }) {
    return _SnapshotGraphState(
      slot: slot,
      nodes: nodes ?? this.nodes,
      canvasBounds: clearCanvasBounds ? null : (canvasBounds ?? this.canvasBounds),
      selectedId: clearSelectedId ? null : (selectedId ?? this.selectedId),
      contours: contours ?? this.contours,
      statusMessage: clearStatusMessage
          ? null
          : (statusMessage ?? this.statusMessage),
    );
  }
}

class _ResultGraphState {
  const _ResultGraphState({
    required this.nodes,
    required this.canvasBounds,
  });

  final List<GraphNodeInfo> nodes;
  final Rect? canvasBounds;

  bool get hasContent =>
      canvasBounds != null && !canvasBounds!.isEmpty && nodes.isNotEmpty;

  Map<int, List<Offset>> get contours => {
        for (final node in nodes) node.id: node.contour,
      };
}

class _GraphDebugScreenState extends State<GraphDebugScreen> {
  static const int _resultSnapshotSlot = 2;

  final NativeCameraService _native = NativeCameraService();
  final TransformationController _liveTransformController =
      TransformationController();

  List<GraphNodeInfo> _nodes = [];
  Rect? _canvasBounds;
  int? _selectedIdA;
  int? _selectedIdB;
  NodeComparison? _comparison;
  String? _comparisonMessage;

  _GraphDebugMode _mode = _GraphDebugMode.live;
  List<_SnapshotGraphState> _snapshots = const [
    _SnapshotGraphState(slot: 0),
    _SnapshotGraphState(slot: 1),
  ];
  GraphSnapshotComparison? _snapshotComparison;
  String? _snapshotComparisonMessage;
  _ResultGraphState? _resultGraph;
  String? _resultGraphMessage;
  int? _capturingSlot;

  int? _dragNodeId;
  bool _isDragging = false;
  Offset? _dragStartCanvas;
  Offset? _dragStartScene;
  Offset? _dragNodeStartCentroid;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _fetchFromCpp();
    });
  }

  @override
  void dispose() {
    _liveTransformController.dispose();
    super.dispose();
  }

  Future<void> _fetchFromCpp() async {
    if (!mounted) return;
    AppLogger.graphDebug('_fetchFromCpp: starting');

    List<GraphNodeInfo> nodes;
    try {
      nodes = _native.getGraphNodes();
      AppLogger.graphDebug('_fetchFromCpp: got ${nodes.length} nodes');
    } catch (e) {
      AppLogger.graphDebug('_fetchFromCpp: getGraphNodes() threw: $e');
      nodes = [];
    }

    Rect? bounds;
    try {
      bounds = _native.getCanvasBounds();
      AppLogger.graphDebug('_fetchFromCpp: bounds=$bounds');
    } catch (e) {
      AppLogger.graphDebug('_fetchFromCpp: getCanvasBounds() threw: $e');
      bounds = null;
    }

    Map<int, List<Offset>> contours;
    try {
      contours = _native.getGraphNodeContours();
      AppLogger.graphDebug('_fetchFromCpp: contours for ${contours.length} nodes');
    } catch (e) {
      AppLogger.graphDebug('_fetchFromCpp: getGraphNodeContours() threw: $e');
      contours = {};
    }

    final enrichedNodes = _mergeContoursIntoNodes(nodes, contours);

    NodeComparison? comp;
    String? compMessage;
    if (_selectedIdA != null && _selectedIdB != null) {
      comp = _native.compareNodes(_selectedIdA!, _selectedIdB!);
      if (comp == null) {
        compMessage = 'Comparison unavailable for the selected pair.';
      }
    }

    if (!mounted) return;
    setState(() {
      _nodes = enrichedNodes;
      _canvasBounds = bounds;
      _comparison = comp;
      _comparisonMessage = compMessage;

      final ids = enrichedNodes.map((n) => n.id).toSet();
      if (_selectedIdA != null && !ids.contains(_selectedIdA)) {
        _selectedIdA = null;
      }
      if (_selectedIdB != null && !ids.contains(_selectedIdB)) {
        _selectedIdB = null;
      }
    });
  }

  List<GraphNodeInfo> _mergeContoursIntoNodes(
    List<GraphNodeInfo> nodes,
    Map<int, List<Offset>> contours,
  ) {
    return nodes
        .map((node) => GraphNodeInfo(
              id: node.id,
              bboxCanvas: node.bboxCanvas,
              centroid: node.centroid,
              area: node.area,
              absenceCount: node.absenceCount,
              lastSeenFrame: node.lastSeenFrame,
              createdFrame: node.createdFrame,
              neighborCount: node.neighborCount,
              canvasOrigin: node.canvasOrigin,
              contour: contours[node.id] ?? const [],
            ))
        .toList();
  }

  void _onTapLiveNode(int nodeId) {
    setState(() {
      if (_selectedIdA == nodeId) {
        _selectedIdA = _selectedIdB;
        _selectedIdB = null;
      } else if (_selectedIdB == nodeId) {
        _selectedIdB = null;
      } else if (_selectedIdA == null) {
        _selectedIdA = nodeId;
      } else if (_selectedIdB == null) {
        _selectedIdB = nodeId;
      } else {
        _selectedIdA = nodeId;
        _selectedIdB = null;
      }
      _comparison = null;
      _comparisonMessage = null;
    });

    if (_selectedIdA != null && _selectedIdB != null) {
      final comp = _native.compareNodes(_selectedIdA!, _selectedIdB!);
      if (!mounted) return;
      setState(() {
        _comparison = comp;
        _comparisonMessage = comp == null
            ? 'Comparison unavailable for the selected pair.'
            : null;
      });
    }
  }

  Future<void> _captureSnapshot(int slot) async {
    if (!mounted) return;
    setState(() {
      _capturingSlot = slot;
      _snapshots = _snapshots
          .map((snapshot) => snapshot.slot == slot
              ? snapshot.copyWith(statusMessage: 'Capturing current frame...')
              : snapshot)
          .toList();
      _snapshotComparison = null;
      _snapshotComparisonMessage = null;
      _resultGraph = null;
      _resultGraphMessage = null;
    });

    bool captured = false;
    try {
      captured = _native.captureGraphSnapshot(slot);
    } catch (e) {
      AppLogger.graphDebug('_captureSnapshot($slot) threw: $e');
    }

    if (!captured) {
      if (!mounted) return;
      setState(() {
        _capturingSlot = null;
        _snapshots = _snapshots
            .map((snapshot) => snapshot.slot == slot
                ? snapshot.copyWith(
                    statusMessage:
                        'Capture failed. Wait for a whiteboard frame and try again.',
                  )
                : snapshot)
            .toList();
      });
      return;
    }

    final nodes = _native.getGraphSnapshotNodes(slot);
    final bounds = _native.getGraphSnapshotCanvasBounds(slot);
    final contours = _native.getGraphSnapshotNodeContours(slot);
    final enriched = _mergeContoursIntoNodes(nodes, contours);

    if (!mounted) return;
    setState(() {
      _capturingSlot = null;
      _snapshots = _snapshots
          .map((snapshot) => snapshot.slot == slot
              ? snapshot.copyWith(
                  nodes: enriched,
                  canvasBounds: bounds,
                  contours: contours,
                  clearSelectedId: snapshot.selectedId != null &&
                      !enriched.any((node) => node.id == snapshot.selectedId),
                  statusMessage: enriched.isEmpty
                      ? 'Captured frame produced no graph nodes.'
                      : 'Captured ${enriched.length} nodes.',
                )
              : snapshot)
          .toList();
    });

    _updateSnapshotComparison();
  }

  void _selectSnapshotNode(int slot, int nodeId) {
    setState(() {
      _snapshots = _snapshots
          .map((snapshot) => snapshot.slot == slot
              ? snapshot.copyWith(selectedId: nodeId, clearStatusMessage: true)
              : snapshot)
          .toList();
      _snapshotComparison = null;
      _snapshotComparisonMessage = null;
      _resultGraph = null;
      _resultGraphMessage = null;
    });
    _updateSnapshotComparison();
  }

  void _combineSnapshots() {
    final left = _snapshotForSlot(0);
    final right = _snapshotForSlot(1);

    if (left.nodes.isEmpty || right.nodes.isEmpty) {
      setState(() {
        _resultGraph = null;
        _resultGraphMessage =
            'Capture both graphs before combining them.';
      });
      return;
    }

    final canUseSelectedSeed =
        left.selectedId != null &&
        right.selectedId != null &&
        left.nodes.any((node) => node.id == left.selectedId) &&
        right.nodes.any((node) => node.id == right.selectedId);

    bool combined = false;
    try {
      combined = _native.combineGraphSnapshots(
        0,
        1,
        anchorIdA: canUseSelectedSeed ? left.selectedId : null,
        anchorIdB: canUseSelectedSeed ? right.selectedId : null,
      );
    } catch (e) {
      AppLogger.graphDebug('_combineSnapshots() threw: $e');
    }

    if (!combined) {
      setState(() {
        _resultGraph = null;
        _resultGraphMessage =
            'Native auto-combine failed. Re-capture both graphs and try again.';
      });
      return;
    }

    final resultGraph = _readNativeResultGraph();

    setState(() {
      _resultGraph = resultGraph;
      _resultGraphMessage = resultGraph == null
          ? 'Native combine produced no visible result graph.'
          : 'Created a combined result graph with ${resultGraph.nodes.length} nodes via native graph matching.';
    });
  }

  void _loadResultGraphIntoSlot(int slot) {
    final resultGraph = _resultGraph;
    if (resultGraph == null || !resultGraph.hasContent) {
      return;
    }

    bool copied = false;
    try {
      copied = _native.copyGraphSnapshot(_resultSnapshotSlot, slot);
    } catch (e) {
      AppLogger.graphDebug('_loadResultGraphIntoSlot($slot) threw: $e');
    }

    if (!copied) {
      setState(() {
        _snapshots = _snapshots
            .map((snapshot) => snapshot.slot == slot
                ? snapshot.copyWith(
                    statusMessage: 'Failed to load the native result graph.',
                  )
                : snapshot)
            .toList();
      });
      return;
    }

    final refreshedSnapshot = _readNativeSnapshot(
      slot,
      clearSelectedId: true,
      statusMessage:
          'Loaded combined result graph (${resultGraph.nodes.length} nodes).',
    );

    setState(() {
      _snapshots = _snapshots
          .map((snapshot) => snapshot.slot == slot
              ? refreshedSnapshot
              : snapshot)
          .toList();
      _snapshotComparison = null;
      _snapshotComparisonMessage = null;
    });
  }

  void _updateSnapshotComparison() {
    final left = _snapshotForSlot(0);
    final right = _snapshotForSlot(1);
    if (left.selectedId == null || right.selectedId == null) {
      if (!mounted) return;
      setState(() {
        _snapshotComparison = null;
        _snapshotComparisonMessage = null;
      });
      return;
    }

    final comparison = _native.compareGraphSnapshotNodes(
      0,
      left.selectedId!,
      1,
      right.selectedId!,
    );

    if (!mounted) return;
    setState(() {
      _snapshotComparison = comparison;
      _snapshotComparisonMessage = comparison == null
          ? 'Comparison unavailable for the selected shapes.'
          : null;
    });
  }

  _SnapshotGraphState _snapshotForSlot(int slot) {
    return _snapshots.firstWhere((snapshot) => snapshot.slot == slot);
  }

  _SnapshotGraphState _readNativeSnapshot(
    int slot, {
    int? selectedId,
    bool clearSelectedId = false,
    String? statusMessage,
  }) {
    final nodes = _native.getGraphSnapshotNodes(slot);
    final bounds = _native.getGraphSnapshotCanvasBounds(slot);
    final contours = _native.getGraphSnapshotNodeContours(slot);
    final enriched = _mergeContoursIntoNodes(nodes, contours);
    final retainedSelectedId = !clearSelectedId &&
            selectedId != null &&
            enriched.any((node) => node.id == selectedId)
        ? selectedId
        : null;

    return _SnapshotGraphState(
      slot: slot,
      nodes: enriched,
      canvasBounds: bounds,
      selectedId: retainedSelectedId,
      contours: contours,
      statusMessage: statusMessage,
    );
  }

  _ResultGraphState? _readNativeResultGraph() {
    final nodes = _native.getGraphSnapshotNodes(_resultSnapshotSlot);
    final bounds = _native.getGraphSnapshotCanvasBounds(_resultSnapshotSlot);
    if (bounds == null || bounds.isEmpty || nodes.isEmpty) {
      return null;
    }

    final contours = _native.getGraphSnapshotNodeContours(_resultSnapshotSlot);
    final enriched = _mergeContoursIntoNodes(nodes, contours);
    if (enriched.isEmpty) {
      return null;
    }

    return _ResultGraphState(
      nodes: enriched,
      canvasBounds: bounds,
    );
  }

  int? _findLiveNodeAtPosition(Offset localPos, Size widgetSize) {
    final transform = _canvasTransformFor(_canvasBounds, widgetSize);
    if (transform == null) return null;

    const hitTolerance = 10.0;
    double bestScore = double.infinity;
    int? bestId;

    for (final node in _nodes) {
      final score = _nodeHitScore(
        node,
        localPos,
        transform,
        tolerance: hitTolerance,
      );
      if (score == null) continue;
      if (score < bestScore) {
        bestScore = score;
        bestId = node.id;
      }
    }
    return bestId;
  }

  _GraphCanvasTransform? _canvasTransformFor(Rect? bounds, Size widgetSize) {
    if (bounds == null || bounds.isEmpty) return null;
    if (widgetSize.width <= 0 || widgetSize.height <= 0) return null;
    return _GraphCanvasTransform.fromBounds(bounds, widgetSize);
  }

  Offset _screenToCanvas(Offset screenPoint, Size widgetSize) {
    final transform = _canvasTransformFor(_canvasBounds, widgetSize);
    if (transform == null) return Offset.zero;
    return transform.screenToCanvas(screenPoint);
  }

  double? _nodeHitScore(
    GraphNodeInfo node,
    Offset screenPoint,
    _GraphCanvasTransform transform, {
    required double tolerance,
  }) {
    final screenBbox = transform.canvasRectToScreen(node.bboxCanvas);
    final expandedBbox = screenBbox.inflate(tolerance);
    if (!expandedBbox.contains(screenPoint)) {
      return null;
    }

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

      if (path.contains(screenPoint)) {
        return 0.0;
      }

      final edgeDistance = _distanceToClosedPolyline(screenPoint, screenContour);
      if (edgeDistance <= tolerance) {
        return edgeDistance;
      }
    }

    if (screenBbox.contains(screenPoint)) {
      return 0.5;
    }

    final centroidDistance =
        (transform.canvasToScreen(node.centroid) - screenPoint).distance;
    if (centroidDistance <= tolerance * 1.5) {
      return centroidDistance + 1.0;
    }

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
    if (abLenSquared <= 0.0001) {
      return (p - a).distance;
    }
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

  @override
  Widget build(BuildContext context) {
    final liveTitle = _mode == _GraphDebugMode.live
        ? 'Graph Debug (${_nodes.length} nodes)'
        : 'Graph Debug Compare';

    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        backgroundColor: Colors.grey[900],
        title: Text(liveTitle),
        actions: [
          SegmentedButton<_GraphDebugMode>(
            segments: const [
              ButtonSegment<_GraphDebugMode>(
                value: _GraphDebugMode.live,
                label: Text('Live'),
                icon: Icon(Icons.hub_outlined),
              ),
              ButtonSegment<_GraphDebugMode>(
                value: _GraphDebugMode.compare,
                label: Text('Compare'),
                icon: Icon(Icons.compare_arrows),
              ),
            ],
            selected: {_mode},
            onSelectionChanged: (selection) {
              setState(() {
                _mode = selection.first;
              });
            },
          ),
          const SizedBox(width: 12),
          if (_mode == _GraphDebugMode.live)
            IconButton(
              icon: const Icon(Icons.refresh),
              tooltip: 'Update from C++',
              onPressed: _fetchFromCpp,
            ),
        ],
      ),
      body: _mode == _GraphDebugMode.live
          ? _buildLiveMode(context)
          : _buildCompareMode(context),
    );
  }

  Widget _buildLiveMode(BuildContext context) {
    return Column(
      children: [
        Container(
          color: Colors.grey[850],
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
          child: Row(
            children: [
              ElevatedButton.icon(
                onPressed: _fetchFromCpp,
                icon: const Icon(Icons.download),
                label: const Text('Update from C++'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.blueGrey[700],
                  foregroundColor: Colors.white,
                ),
              ),
              const SizedBox(width: 16),
              Text(
                '${_nodes.length} nodes',
                style: const TextStyle(color: Colors.grey, fontSize: 13),
              ),
            ],
          ),
        ),
        Expanded(
          child: Row(
            children: [
              Expanded(child: _buildLiveCanvasView()),
              SizedBox(
                width: 320,
                child: _buildLiveSidebar(context),
              ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildLiveCanvasView() {
    return LayoutBuilder(
      builder: (context, constraints) {
        final widgetSize = Size(constraints.maxWidth, constraints.maxHeight);
        return Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (event) {
            final scenePoint = _liveTransformController.toScene(event.localPosition);
            final nodeId = _findLiveNodeAtPosition(scenePoint, widgetSize);
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

            final scenePoint = _liveTransformController.toScene(event.localPosition);
            final sceneDelta = scenePoint - _dragStartScene!;
            final canvasPoint = _screenToCanvas(scenePoint, widgetSize);
            final canvasDelta = canvasPoint - _dragStartCanvas!;

            if (!_isDragging && sceneDelta.distance < 4.0) {
              return;
            }

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
            _resetDragState();
            if (!wasDragging && tappedNodeId != null) {
              _onTapLiveNode(tappedNodeId);
            }
          },
          onPointerCancel: (_) => _resetDragState(),
          child: InteractiveViewer(
            transformationController: _liveTransformController,
            boundaryMargin: const EdgeInsets.all(200),
            minScale: 0.1,
            maxScale: 10.0,
            panEnabled: _dragNodeId == null,
            scaleEnabled: _dragNodeId == null,
            child: SizedBox(
              width: widgetSize.width,
              height: widgetSize.height,
              child: CustomPaint(
                painter: VectorGraphPainter(
                  nodes: _nodes,
                  canvasBounds: _canvasBounds,
                  selectedIds: {_selectedIdA, _selectedIdB}.whereType<int>().toSet(),
                  selectionColorForNode: (nodeId) {
                    if (nodeId == _selectedIdA) {
                      return const Color(0xFF4488FF);
                    }
                    if (nodeId == _selectedIdB) {
                      return const Color(0xFFFF8800);
                    }
                    return null;
                  },
                ),
                size: widgetSize,
              ),
            ),
          ),
        );
      },
    );
  }

  Widget _buildLiveSidebar(BuildContext context) {
    return Container(
      color: Colors.grey[900],
      padding: const EdgeInsets.all(12),
      child: ListView(
        children: [
          Text(
            'Selection',
            style: Theme.of(context)
                .textTheme
                .titleMedium
                ?.copyWith(color: Colors.white),
          ),
          const SizedBox(height: 8),
          if (_selectedIdA == null && _selectedIdB == null)
            const Text(
              'Tap a node to select',
              style: TextStyle(color: Colors.grey),
            ),
          if (_selectedIdA != null) _buildNodeCard(_selectedIdA!, Colors.blue),
          if (_selectedIdB != null) ...[
            const SizedBox(height: 8),
            _buildNodeCard(_selectedIdB!, Colors.orange),
          ],
          if (_selectedIdA != null && _selectedIdB != null) ...[
            const Divider(color: Colors.grey),
            Text(
              'Comparison',
              style: Theme.of(context)
                  .textTheme
                  .titleMedium
                  ?.copyWith(color: Colors.white),
            ),
            const SizedBox(height: 8),
            if (_comparison != null)
              _buildComparisonCard(_comparison!)
            else
              _buildMessageCard(
                _comparisonMessage ??
                    'Comparison is not available yet for the selected pair.',
              ),
          ],
        ],
      ),
    );
  }

  Widget _buildCompareMode(BuildContext context) {
    final left = _snapshotForSlot(0);
    final right = _snapshotForSlot(1);

    return LayoutBuilder(
      builder: (context, constraints) {
        final isNarrowLayout = constraints.maxWidth < 1000;
        final panelHeight = math.max(
          isNarrowLayout ? 360.0 : 440.0,
          constraints.maxHeight * (isNarrowLayout ? 0.48 : 0.56),
        );

        return Scrollbar(
          thumbVisibility: true,
          child: SingleChildScrollView(
            child: ConstrainedBox(
              constraints: BoxConstraints(minHeight: constraints.maxHeight),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Container(
                    color: Colors.grey[850],
                    padding: const EdgeInsets.symmetric(
                      horizontal: 12,
                      vertical: 8,
                    ),
                    child: Row(
                      children: [
                        const Icon(
                          Icons.info_outline,
                          color: Colors.grey,
                          size: 18,
                        ),
                        const SizedBox(width: 8),
                        const Expanded(
                          child: Text(
                            'Capture two frames. Tap one shape on each side to compare them, or press Combine Graphs to auto-align and merge them.',
                            style: TextStyle(color: Colors.grey, fontSize: 13),
                          ),
                        ),
                      ],
                    ),
                  ),
                  Padding(
                    padding: const EdgeInsets.all(12),
                    child: isNarrowLayout
                        ? Column(
                            children: [
                              SizedBox(
                                height: panelHeight,
                                child: _buildSnapshotPanel(
                                  context,
                                  left,
                                  Colors.teal,
                                  'Left Graph',
                                ),
                              ),
                              const SizedBox(height: 12),
                              SizedBox(
                                height: panelHeight,
                                child: _buildSnapshotPanel(
                                  context,
                                  right,
                                  Colors.deepOrange,
                                  'Right Graph',
                                ),
                              ),
                            ],
                          )
                        : SizedBox(
                            height: panelHeight,
                            child: Row(
                              children: [
                                Expanded(
                                  child: _buildSnapshotPanel(
                                    context,
                                    left,
                                    Colors.teal,
                                    'Left Graph',
                                  ),
                                ),
                                Container(width: 1, color: Colors.grey[850]),
                                Expanded(
                                  child: _buildSnapshotPanel(
                                    context,
                                    right,
                                    Colors.deepOrange,
                                    'Right Graph',
                                  ),
                                ),
                              ],
                            ),
                          ),
                  ),
                  Padding(
                    padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
                    child: _buildSnapshotComparisonBar(context),
                  ),
                ],
              ),
            ),
          ),
        );
      },
    );
  }

  Widget _buildSnapshotPanel(
    BuildContext context,
    _SnapshotGraphState snapshot,
    Color accent,
    String title,
  ) {
    final isCapturing = _capturingSlot == snapshot.slot;
    final canLoadResult = _resultGraph?.hasContent ?? false;
    return Container(
      color: Colors.black,
      child: Column(
        children: [
          Container(
            color: Colors.grey[900],
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            child: Row(
              children: [
                Container(width: 10, height: 10, color: accent),
                const SizedBox(width: 10),
                Expanded(
                  child: Text(
                    '$title (${snapshot.nodes.length} nodes)',
                    style: const TextStyle(
                      color: Colors.white,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ),
                Wrap(
                  spacing: 8,
                  runSpacing: 8,
                  alignment: WrapAlignment.end,
                  children: [
                    ElevatedButton.icon(
                      onPressed:
                          isCapturing ? null : () => _captureSnapshot(snapshot.slot),
                      icon: Icon(
                        isCapturing ? Icons.hourglass_top : Icons.camera_alt_outlined,
                      ),
                      label: Text(isCapturing ? 'Capturing...' : 'Capture Frame'),
                      style: ElevatedButton.styleFrom(
                        backgroundColor: accent.withValues(alpha: 0.85),
                        foregroundColor: Colors.white,
                      ),
                    ),
                    OutlinedButton.icon(
                      onPressed: canLoadResult
                          ? () => _loadResultGraphIntoSlot(snapshot.slot)
                          : null,
                      icon: const Icon(Icons.account_tree_outlined),
                      label: const Text('Load Result Graph'),
                      style: OutlinedButton.styleFrom(
                        foregroundColor: Colors.white,
                        side: BorderSide(
                          color: accent.withValues(alpha: 0.85),
                        ),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          Expanded(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: _buildSnapshotCanvas(snapshot, accent),
            ),
          ),
          Container(
            color: Colors.grey[900],
            padding: const EdgeInsets.all(12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (snapshot.statusMessage != null)
                  Padding(
                    padding: const EdgeInsets.only(bottom: 8),
                    child: Text(
                      snapshot.statusMessage!,
                      style: const TextStyle(color: Colors.grey, fontSize: 12),
                    ),
                  ),
                if (snapshot.selectedId != null)
                  _buildSnapshotNodeCard(snapshot, accent)
                else
                  const Text(
                    'Select a shape in this graph.',
                    style: TextStyle(color: Colors.grey, fontSize: 12),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildSnapshotCanvas(_SnapshotGraphState snapshot, Color accent) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final widgetSize = Size(constraints.maxWidth, constraints.maxHeight);
        return GestureDetector(
          behavior: HitTestBehavior.opaque,
          onTapUp: (details) {
            final transform = _canvasTransformFor(snapshot.canvasBounds, widgetSize);
            if (transform == null) return;

            const hitTolerance = 10.0;
            double bestScore = double.infinity;
            int? bestId;
            for (final node in snapshot.nodes) {
              final score = _nodeHitScore(
                node,
                details.localPosition,
                transform,
                tolerance: hitTolerance,
              );
              if (score == null) continue;
              if (score < bestScore) {
                bestScore = score;
                bestId = node.id;
              }
            }
            if (bestId != null) {
              _selectSnapshotNode(snapshot.slot, bestId);
            }
          },
          child: CustomPaint(
            painter: VectorGraphPainter(
              nodes: snapshot.nodes,
              canvasBounds: snapshot.canvasBounds,
              selectedIds: snapshot.selectedId == null ? const {} : {snapshot.selectedId!},
              selectionColorForNode: (nodeId) =>
                  nodeId == snapshot.selectedId ? accent : null,
            ),
            size: widgetSize,
          ),
        );
      },
    );
  }

  Widget _buildSnapshotNodeCard(_SnapshotGraphState snapshot, Color accent) {
    final node = snapshot.nodes.where((n) => n.id == snapshot.selectedId).firstOrNull;
    if (node == null) {
      return const Text(
        'Selected node no longer exists in this capture.',
        style: TextStyle(color: Colors.grey, fontSize: 12),
      );
    }

    return Card(
      color: Colors.grey[850],
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(width: 12, height: 12, color: accent),
                const SizedBox(width: 8),
                Text(
                  'Node ${node.id}',
                  style: TextStyle(
                    color: accent,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 6),
            _infoRow('BBox',
                '${node.bboxCanvas.width.toInt()}x${node.bboxCanvas.height.toInt()}'),
            _infoRow(
                'Centroid', '${node.centroid.dx.toInt()}, ${node.centroid.dy.toInt()}'),
            _infoRow('Area', '${node.area.toInt()}'),
            _infoRow('Contour pts', '${node.contour.length}'),
          ],
        ),
      ),
    );
  }

  Widget _buildSnapshotComparisonBar(BuildContext context) {
    final left = _snapshotForSlot(0);
    final right = _snapshotForSlot(1);
    final canCompare = left.selectedId != null && right.selectedId != null;
    final canCombine = left.nodes.isNotEmpty && right.nodes.isNotEmpty;
    final resultGraph = _resultGraph;

    return Container(
      color: Colors.grey[950],
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: Text(
                  'Cross-Graph Comparison',
                  style: Theme.of(context).textTheme.titleMedium?.copyWith(
                        color: Colors.white,
                        fontWeight: FontWeight.w700,
                      ),
                ),
              ),
              ElevatedButton.icon(
                onPressed: canCombine ? _combineSnapshots : null,
                icon: const Icon(Icons.merge_type),
                label: Text(
                  resultGraph == null ? 'Combine Graphs' : 'Recombine Graphs',
                ),
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.blueGrey[700],
                  foregroundColor: Colors.white,
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          if (canCompare)
            (_snapshotComparison != null
                ? _buildSnapshotComparisonCard(_snapshotComparison!)
                : _buildMessageCard(
                    _snapshotComparisonMessage ??
                        'Comparison unavailable for the selected shapes.',
                  ))
          else
            const Text(
              'Choose one shape on the left and one on the right to compare them.',
              style: TextStyle(color: Colors.grey),
            ),
          const SizedBox(height: 16),
          Container(height: 1, color: Colors.grey[850]),
          const SizedBox(height: 16),
          _buildResultGraphSection(),
        ],
      ),
    );
  }

  Widget _buildResultGraphSection() {
    final resultGraph = _resultGraph;
    final hasResult = resultGraph?.hasContent ?? false;

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          'Combined Result Graph',
          style: Theme.of(context).textTheme.titleMedium?.copyWith(
                color: Colors.white,
                fontWeight: FontWeight.w700,
              ),
        ),
        const SizedBox(height: 8),
        if (_resultGraphMessage != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Text(
              _resultGraphMessage!,
              style: TextStyle(
                color: hasResult ? Colors.grey[400] : Colors.orange,
                fontSize: 12,
              ),
            ),
          ),
        if (hasResult)
          Container(
            height: 340,
            decoration: BoxDecoration(
              color: Colors.black,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: Colors.grey[850]!),
            ),
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: LayoutBuilder(
                builder: (context, constraints) {
                  final widgetSize = Size(
                    constraints.maxWidth,
                    constraints.maxHeight,
                  );
                  return CustomPaint(
                    painter: VectorGraphPainter(
                      nodes: resultGraph!.nodes,
                      canvasBounds: resultGraph.canvasBounds,
                      selectedIds: const {},
                      selectionColorForNode: (_) => null,
                    ),
                    size: widgetSize,
                  );
                },
              ),
            ),
          )
        else
          Container(
            width: double.infinity,
            padding: const EdgeInsets.all(16),
            decoration: BoxDecoration(
              color: Colors.grey[900],
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: Colors.grey[850]!),
            ),
            child: Text(
              'Press Combine Graphs to auto-align and merge the two captured graphs here.',
              style: const TextStyle(color: Colors.grey, fontSize: 12),
            ),
          ),
      ],
    );
  }

  Widget _buildNodeCard(int nodeId, Color accent) {
    final node = _nodes.where((n) => n.id == nodeId).firstOrNull;
    if (node == null) {
      return Card(
        color: Colors.grey[800],
        child: const Padding(
          padding: EdgeInsets.all(8),
          child: Text(
            'Selected node was removed.',
            style: TextStyle(color: Colors.grey),
          ),
        ),
      );
    }

    return Card(
      color: Colors.grey[800],
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(width: 12, height: 12, color: accent),
                const SizedBox(width: 8),
                Text(
                  'Node ${node.id}',
                  style: TextStyle(
                    color: accent,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 4),
            _infoRow('BBox', '${node.bboxCanvas.left.toInt()}, '
                '${node.bboxCanvas.top.toInt()} '
                '${node.bboxCanvas.width.toInt()}x'
                '${node.bboxCanvas.height.toInt()}'),
            _infoRow('Centroid',
                '${node.centroid.dx.toInt()}, ${node.centroid.dy.toInt()}'),
            _infoRow('Area', '${node.area.toInt()}'),
            _infoRow('Contour pts', '${node.contour.length}'),
            _infoRow('Absence', '${node.absenceCount}'),
            _infoRow('Created', 'frame ${node.createdFrame}'),
            _infoRow('Last seen', 'frame ${node.lastSeenFrame}'),
            _infoRow('Neighbors', '${node.neighborCount}'),
            _infoRow('Match dist', node.matchDistance < 0
                ? 'n/a'
                : '${node.matchDistance}'),
          ],
        ),
      ),
    );
  }

  Widget _infoRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 1),
      child: Row(
        children: [
          SizedBox(
            width: 80,
            child: Text(
              label,
              style: const TextStyle(color: Colors.grey, fontSize: 12),
            ),
          ),
          Expanded(
            child: Text(
              value,
              style: const TextStyle(color: Colors.white, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildComparisonCard(NodeComparison comp) {
    final shapeVerdict = comp.shapeDistance < 0.25
        ? _Verdict.same
        : comp.shapeDistance < 0.5
            ? _Verdict.maybe
            : _Verdict.different;
    final overlapVerdict = comp.maskOverlapRatio > 0.6
        ? _Verdict.same
        : comp.maskOverlapRatio > 0.15
            ? _Verdict.maybe
            : _Verdict.different;

    return Card(
      color: Colors.grey[800],
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _compRow(
              'OpenCV shape',
              'I2 = ${comp.shapeDistance.toStringAsFixed(4)}',
              shapeVerdict,
            ),
            _compRow(
              'Centroid dist',
              '${comp.centroidDistance.toStringAsFixed(1)} px',
              null,
            ),
            _compRow(
              'BBox intersect',
              '${comp.bboxIntersectionArea.toInt()} px²',
              null,
            ),
            _compRow(
              'AND pixels',
              '${comp.andOverlapPixels.toInt()} px',
              null,
            ),
            _compRow(
              'AND ratio',
              '${(comp.maskOverlapRatio * 100).toStringAsFixed(1)}% of smaller mask',
              overlapVerdict,
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildSnapshotComparisonCard(GraphSnapshotComparison comp) {
    final shapeVerdict = comp.shapeDistance < 0.25
        ? _Verdict.same
        : comp.shapeDistance < 0.5
            ? _Verdict.maybe
            : _Verdict.different;
    final edgeVerdict = comp.averageEdgeSimilarity > 0.9
        ? _Verdict.same
        : comp.averageEdgeSimilarity > 0.75
            ? _Verdict.maybe
            : _Verdict.different;

    return Card(
      color: Colors.grey[850],
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _compRow(
              'OpenCV shape',
              'I2 = ${comp.shapeDistance.toStringAsFixed(4)}',
              shapeVerdict,
            ),
            _compRow(
              'Width ratio',
              comp.widthRatio.toStringAsFixed(4),
              null,
            ),
            _compRow(
              'Height ratio',
              comp.heightRatio.toStringAsFixed(4),
              null,
            ),
            _compRow(
              'Long edge sim',
              comp.longEdgeSimilarity.toStringAsFixed(4),
              edgeVerdict,
            ),
            _compRow(
              'Short edge sim',
              comp.shortEdgeSimilarity.toStringAsFixed(4),
              edgeVerdict,
            ),
            _compRow(
              'Avg edge sim',
              comp.averageEdgeSimilarity.toStringAsFixed(4),
              edgeVerdict,
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildMessageCard(String message) {
    return Card(
      color: Colors.grey[800],
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Text(
          message,
          style: const TextStyle(color: Colors.orange, fontSize: 12),
        ),
      ),
    );
  }

  Widget _compRow(String label, String value, _Verdict? verdict) {
    Color valueColor = Colors.white;
    if (verdict == _Verdict.same) valueColor = Colors.green;
    if (verdict == _Verdict.maybe) valueColor = Colors.yellow;
    if (verdict == _Verdict.different) valueColor = Colors.red;

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        children: [
          SizedBox(
            width: 110,
            child: Text(
              label,
              style: const TextStyle(color: Colors.grey, fontSize: 12),
            ),
          ),
          Expanded(
            child: Text(
              value,
              style: TextStyle(color: valueColor, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }
}

enum _Verdict { same, maybe, different }

class VectorGraphPainter extends CustomPainter {
  VectorGraphPainter({
    required this.nodes,
    required this.canvasBounds,
    required this.selectedIds,
    required this.selectionColorForNode,
  });

  final List<GraphNodeInfo> nodes;
  final Rect? canvasBounds;
  final Set<int> selectedIds;
  final Color? Function(int nodeId) selectionColorForNode;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(
      Rect.fromLTWH(0, 0, size.width, size.height),
      Paint()..color = const Color(0xFF000000),
    );

    if (canvasBounds == null || canvasBounds!.isEmpty || nodes.isEmpty) return;

    final transform = _GraphCanvasTransform.fromBounds(canvasBounds!, size);

    final shapeFillPaint = Paint()
      ..color = const Color(0xFFFFFFFF)
      ..style = PaintingStyle.fill;

    final shapeStrokePaint = Paint()
      ..color = const Color(0xAAFFFFFF)
      ..strokeWidth = 1.0
      ..style = PaintingStyle.stroke;

    final centroidPaint = Paint()
      ..color = const Color(0xFFFF0000)
      ..style = PaintingStyle.fill;

    final textPainter = TextPainter(textDirection: TextDirection.ltr);

    for (final node in nodes) {
      final screenCentroid = transform.canvasToScreen(node.centroid);

      if (node.contour.length >= 3) {
        final path = ui.Path();
        final first = transform.canvasToScreen(node.contour[0]);
        path.moveTo(first.dx, first.dy);
        for (int i = 1; i < node.contour.length; i++) {
          final pt = transform.canvasToScreen(node.contour[i]);
          path.lineTo(pt.dx, pt.dy);
        }
        path.close();
        canvas.drawPath(path, shapeFillPaint);

        if (selectedIds.contains(node.id)) {
          final selectionPaint = Paint()
            ..color = selectionColorForNode(node.id) ?? const Color(0xFF00BCD4)
            ..strokeWidth = 2.5
            ..style = PaintingStyle.stroke;
          canvas.drawPath(path, selectionPaint);
        }
      } else {
        final screenBbox = transform.canvasRectToScreen(node.bboxCanvas);
        canvas.drawRect(screenBbox, shapeStrokePaint);
        if (selectedIds.contains(node.id)) {
          final selectionPaint = Paint()
            ..color = selectionColorForNode(node.id) ?? const Color(0xFF00BCD4)
            ..strokeWidth = 2.5
            ..style = PaintingStyle.stroke;
          canvas.drawRect(screenBbox, selectionPaint);
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
  bool shouldRepaint(VectorGraphPainter oldDelegate) {
    return oldDelegate.nodes != nodes ||
        oldDelegate.canvasBounds != canvasBounds ||
        oldDelegate.selectedIds != selectedIds;
  }
}

class _GraphCanvasTransform {
  const _GraphCanvasTransform({
    required this.bounds,
    required this.scale,
    required this.contentOffset,
  });

  factory _GraphCanvasTransform.fromBounds(Rect bounds, Size viewportSize) {
    final scale = math.min(
      viewportSize.width / bounds.width,
      viewportSize.height / bounds.height,
    );
    final contentSize = Size(bounds.width * scale, bounds.height * scale);
    final contentOffset = Offset(
      (viewportSize.width - contentSize.width) / 2,
      (viewportSize.height - contentSize.height) / 2,
    );
    return _GraphCanvasTransform(
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
