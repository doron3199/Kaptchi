import 'dart:async';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';

class StrokePoint {
  final Offset point;
  final double pressure;
  const StrokePoint(this.point, this.pressure);
}

class Stroke {
  final List<StrokePoint> points;
  final Color color;
  final double width;
  final bool isEraser;
  const Stroke({
    required this.points,
    required this.color,
    required this.width,
    this.isEraser = false,
  });
}

/// Helper to calculate the rect where content fits inside a container (BoxFit.contain)
Rect getContainRect(Size containerSize, Size contentSize) {
  final double renderAspect = containerSize.width / containerSize.height;
  final double contentAspect = contentSize.width / contentSize.height;

  double drawWidth, drawHeight, dx, dy;

  if (renderAspect > contentAspect) {
    drawHeight = containerSize.height;
    drawWidth = drawHeight * contentAspect;
    dx = (containerSize.width - drawWidth) / 2;
    dy = 0;
  } else {
    drawWidth = containerSize.width;
    drawHeight = drawWidth / contentAspect;
    dx = 0;
    dy = (containerSize.height - drawHeight) / 2;
  }
  return Rect.fromLTWH(dx, dy, drawWidth, drawHeight);
}

/// Shared drawing logic for strokes
void drawStrokes(Canvas canvas, Size size, List<Stroke> strokes) {
  final paint = Paint()
    ..style = PaintingStyle.stroke
    ..strokeCap = StrokeCap.round
    ..strokeJoin = StrokeJoin.round;

  for (final stroke in strokes) {
    if (stroke.points.length < 2) continue;

    paint.color = stroke.color;
    paint.blendMode = stroke.isEraser ? BlendMode.clear : BlendMode.srcOver;

    for (int i = 0; i < stroke.points.length - 1; i++) {
      final p1 = stroke.points[i];
      final p2 = stroke.points[i + 1];

      double pressure = (p1.pressure + p2.pressure) / 2;
      if (pressure == 0) pressure = 0.5;
      if (pressure > 1.0) pressure = 1.0;

      paint.strokeWidth =
          stroke.width *
          (stroke.isEraser ? 2.0 : 1.0) *
          (pressure * 2) *
          (size.width / 1000.0);

      canvas.drawLine(
        Offset(p1.point.dx * size.width, p1.point.dy * size.height),
        Offset(p2.point.dx * size.width, p2.point.dy * size.height),
        paint,
      );
    }
  }
}

class ImageEditorScreen extends StatefulWidget {
  final Uint8List imageBytes;

  const ImageEditorScreen({super.key, required this.imageBytes});

  @override
  State<ImageEditorScreen> createState() => _ImageEditorScreenState();
}

class _ImageEditorScreenState extends State<ImageEditorScreen> {
  ui.Image? _image;
  final List<Stroke> _strokes = [];
  Stroke? _currentStroke;

  Color _selectedColor = Colors.red;
  double _baseStrokeWidth = 5.0;
  bool _isEraserMode = false;

  // To handle coordinate mapping
  final GlobalKey _imageKey = GlobalKey();

  // Transform State (Feature 9)
  Offset _offset = Offset.zero;
  double _scale = 1.0;
  Offset _baseOffset = Offset.zero; // For zoom calculation
  Offset _lastFocalPoint = Offset.zero; // Initialized to Offset.zero
  double _baseScale = 1.0;

  // Modes
  bool _isHandMode = false; // false = Draw, true = Pan/Zoom

  @override
  void initState() {
    super.initState();
    _loadImage();
  }

  Future<void> _loadImage() async {
    final codec = await ui.instantiateImageCodec(widget.imageBytes);
    final frame = await codec.getNextFrame();
    setState(() {
      _image = frame.image;
    });
  }

  // Normalize point to 0..1 range relative to the image rect
  Offset? _normalizePoint(
    Offset localPosition,
    Size renderSize,
    Size imageSize,
  ) {
    // Calculate the rect where the image is actually drawn within the renderSize (contain fit)
    final rect = getContainRect(renderSize, imageSize);
    final dx = rect.left;
    final dy = rect.top;
    final drawWidth = rect.width;
    final drawHeight = rect.height;

    final double unscaledDx = (localPosition.dx - _offset.dx) / _scale;
    final double unscaledDy = (localPosition.dy - _offset.dy) / _scale;

    if (unscaledDx < dx ||
        unscaledDx > dx + drawWidth ||
        unscaledDy < dy ||
        unscaledDy > dy + drawHeight) {
      return null;
    }

    return Offset(
      (unscaledDx - dx) / drawWidth,
      (unscaledDy - dy) / drawHeight,
    );
  }

  int _pointerCount = 0;

  void _startStroke(Offset localPos, double pressure) {
    final RenderBox? box =
        _imageKey.currentContext?.findRenderObject() as RenderBox?;
    if (box == null) return;

    final normalized = _normalizePoint(
      localPos,
      box.size,
      Size(_image!.width.toDouble(), _image!.height.toDouble()),
    );

    if (normalized != null) {
      if (pressure <= 0 || pressure >= 1.0) pressure = 0.5;
      setState(() {
        _currentStroke = Stroke(
          color: _selectedColor,
          width: _baseStrokeWidth,
          points: [StrokePoint(normalized, pressure)],
          isEraser: _isEraserMode,
        );
        _strokes.add(_currentStroke!);
      });
    }
  }

  void _addStrokePoint(Offset localPos, double pressure) {
    if (_currentStroke == null) return;

    final RenderBox? box =
        _imageKey.currentContext?.findRenderObject() as RenderBox?;
    if (box == null) return;

    final normalized = _normalizePoint(
      localPos,
      box.size,
      Size(_image!.width.toDouble(), _image!.height.toDouble()),
    );

    if (normalized != null) {
      if (pressure <= 0 || pressure >= 1.0) pressure = 0.5;
      setState(() {
        _currentStroke!.points.add(StrokePoint(normalized, pressure));
      });
    }
  }

  void _onPointerDownListener(PointerDownEvent event) {
    _pointerCount++;
    if (_pointerCount == 1) {
      // Start drawing potentially
      final RenderBox? box =
          _imageKey.currentContext?.findRenderObject() as RenderBox?;
      if (box != null) {
        final localPos = box.globalToLocal(event.position);
        _startStroke(localPos, event.pressure);
      }
    } else {
      // Multi-touch started, cancel stroke
      _currentStroke = null;
    }
  }

  void _onPointerMoveListener(PointerMoveEvent event) {
    if (_pointerCount == 1 && _currentStroke != null) {
      final RenderBox? box =
          _imageKey.currentContext?.findRenderObject() as RenderBox?;
      if (box != null) {
        final localPos = box.globalToLocal(event.position);
        _addStrokePoint(localPos, event.pressure);
      }
    }
  }

  void _onPointerUpListener(PointerUpEvent event) {
    _pointerCount--;
    if (_currentStroke != null) {
      _currentStroke = null;
    }
  }

  void _onPointerCancelListener(PointerCancelEvent event) {
    _pointerCount--;
    _currentStroke = null;
  }

  void _undo() {
    setState(() {
      if (_strokes.isNotEmpty) {
        _strokes.removeLast();
      }
    });
  }

  Future<void> _saveImage() async {
    if (_image == null) return;

    final recorder = ui.PictureRecorder();
    final canvas = Canvas(recorder);

    // Draw original image
    canvas.drawImage(_image!, Offset.zero, Paint());

    // Save layer for strokes to allow eraser to work properly (clearing strokes but not image)
    canvas.saveLayer(
      Rect.fromLTWH(0, 0, _image!.width.toDouble(), _image!.height.toDouble()),
      Paint(),
    );

    // Draw strokes
    drawStrokes(
      canvas,
      Size(_image!.width.toDouble(), _image!.height.toDouble()),
      _strokes,
    );

    canvas.restore();

    final picture = recorder.endRecording();
    final img = await picture.toImage(_image!.width, _image!.height);
    final byteData = await img.toByteData(format: ui.ImageByteFormat.png);

    if (byteData != null && mounted) {
      Navigator.pop(context, byteData.buffer.asUint8List());
    } else {
      // Debug logic: ensure bytes are returned
      debugPrint("Save failed or byteData null");
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        title: const Text('Edit Image'),
        backgroundColor: Colors.black,
        actions: [
          // Top actions removed (moved to bottom)
        ],
      ),
      body: Column(
        children: [
          Expanded(
            child: _image == null
                ? const Center(child: CircularProgressIndicator())
                : GestureDetector(
                    onScaleStart: (details) {
                      if (!_isHandMode) return;
                      _baseScale = _scale;
                      _baseOffset = _offset; // Capture base offset
                      _lastFocalPoint = details.localFocalPoint;
                    },
                    onScaleUpdate: (details) {
                      if (!_isHandMode) return;
                      setState(() {
                        // Zoom/Pan logic
                        double newScale = (_baseScale * details.scale).clamp(
                          1.0,
                          5.0,
                        );

                        // Calculate zoom so that focal point remains under fingers
                        Offset focal = details.localFocalPoint;
                        Offset focalStartInImage =
                            (_lastFocalPoint - _baseOffset) / _baseScale;

                        _offset = focal - (focalStartInImage * newScale);
                        _scale = newScale;
                      });
                    },
                    child: Listener(
                      behavior: HitTestBehavior.opaque,
                      onPointerDown: (event) {
                        if (_isHandMode) return;
                        _onPointerDownListener(event);
                      },
                      onPointerMove: (event) {
                        if (_isHandMode) return;
                        _onPointerMoveListener(event);
                      },
                      onPointerUp: (event) {
                        if (_isHandMode) return;
                        _onPointerUpListener(event);
                      },
                      onPointerCancel: (event) {
                        if (_isHandMode) return;
                        _onPointerCancelListener(event);
                      },
                      child: ClipRect(
                        child: SizedBox(
                          key: _imageKey,
                          width: double.infinity,
                          height: double.infinity,
                          child: CustomPaint(
                            painter: _EditorPainter(
                              _image!,
                              _strokes,
                              _offset,
                              _scale,
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
          ),
          // Consolidated Toolbar
          // Single Consolidated Toolbar
          Container(
            color: Colors.black,
            padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 8),
            child: SafeArea(
              child: SizedBox(
                height: 50,
                child: Row(
                  children: [
                    // Colors
                    Expanded(
                      child: ListView(
                        scrollDirection: Axis.horizontal,
                        children: [
                          _buildColorBtn(Colors.red),
                          _buildColorBtn(Colors.white),
                          _buildColorBtn(Colors.black),
                          _buildColorBtn(Colors.blue),
                          _buildColorBtn(Colors.green),
                          _buildColorBtn(Colors.yellow),
                          GestureDetector(
                            onTap: () =>
                                setState(() => _isEraserMode = !_isEraserMode),
                            child: Container(
                              margin: const EdgeInsets.all(8),
                              width: 32,
                              height: 32,
                              decoration: BoxDecoration(
                                color: _isEraserMode
                                    ? Colors.white
                                    : Colors.grey[800],
                                shape: BoxShape.circle,
                                border: Border.all(
                                  color: Colors.white,
                                  width: 1,
                                ),
                              ),
                              child: Icon(
                                Icons.cleaning_services,
                                size: 16,
                                color: _isEraserMode
                                    ? Colors.black
                                    : Colors.white,
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                    // Vertical Divider
                    const VerticalDivider(color: Colors.white24, width: 20),
                    // Thickness Slider (Compact)
                    SizedBox(
                      width: 150,
                      child: Slider(
                        value: _baseStrokeWidth,
                        min: 1.0,
                        max: 20.0, // Increased max width by 4x (was 20)
                        onChanged: (v) => setState(() => _baseStrokeWidth = v),
                        activeColor: Colors.white,
                        inactiveColor: Colors.grey,
                      ),
                    ),
                    // Actions
                    IconButton(
                      icon: Icon(
                        Icons.undo,
                        color: _strokes.isNotEmpty ? Colors.white : Colors.grey,
                      ),
                      onPressed: _strokes.isNotEmpty ? _undo : null,
                    ),
                    IconButton(
                      icon: Icon(
                        Icons.pan_tool,
                        color: _isHandMode ? Colors.blue : Colors.white,
                      ),
                      onPressed: () =>
                          setState(() => _isHandMode = !_isHandMode),
                    ),
                    IconButton(
                      icon: const Icon(Icons.check, color: Colors.green),
                      onPressed: _saveImage,
                    ),
                  ],
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildColorBtn(Color color) {
    return GestureDetector(
      onTap: () => setState(() {
        _selectedColor = color;
        _isEraserMode = false; // Turn off eraser when picking color
      }),
      child: Container(
        margin: const EdgeInsets.all(4),
        width: 32,
        height: 32,
        decoration: BoxDecoration(
          color: color,
          shape: BoxShape.circle,
          border: (!_isEraserMode && _selectedColor == color)
              ? Border.all(color: Colors.white, width: 2)
              : null,
        ),
      ),
    );
  }
}

class _EditorPainter extends CustomPainter {
  final ui.Image image;
  final List<Stroke> strokes;
  final Offset offset;
  final double scale;

  _EditorPainter(this.image, this.strokes, this.offset, this.scale);

  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    canvas.translate(offset.dx, offset.dy);
    canvas.scale(scale);

    // 1. Draw Image (Contain fit)
    final imageRect = getContainRect(
      size,
      Size(image.width.toDouble(), image.height.toDouble()),
    );

    paintImage(
      canvas: canvas,
      rect: imageRect,
      image: image,
      fit: BoxFit.contain,
    );

    // 2. Draw Strokes
    // Use saveLayer to allow eraser to clear strokes without clearing the image
    canvas.saveLayer(imageRect, Paint());

    // Clip to image rect to ensure no drawing outside (optional but good)
    canvas.clipRect(imageRect);
    canvas.translate(imageRect.left, imageRect.top); // Move to image origin

    drawStrokes(canvas, imageRect.size, strokes);

    canvas.restore();
    canvas.restore(); // Restore transform
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}
