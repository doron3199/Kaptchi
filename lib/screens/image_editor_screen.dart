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
  Offset? _normalizePoint(Offset localPosition, Size renderSize, Size imageSize) {
    // Calculate the rect where the image is actually drawn within the renderSize (contain fit)
    final double renderAspect = renderSize.width / renderSize.height;
    final double imageAspect = imageSize.width / imageSize.height;

    double drawWidth, drawHeight, dx, dy;

    if (renderAspect > imageAspect) {
      // Render is wider than image: Image is height-constrained
      drawHeight = renderSize.height;
      drawWidth = drawHeight * imageAspect;
      dx = (renderSize.width - drawWidth) / 2;
      dy = 0;
    } else {
      // Render is taller than image: Image is width-constrained
      drawWidth = renderSize.width;
      drawHeight = drawWidth / imageAspect;
      dx = 0;
      dy = (renderSize.height - drawHeight) / 2;
    }

    // Check if point is inside the image rect
    if (localPosition.dx < dx || localPosition.dx > dx + drawWidth ||
        localPosition.dy < dy || localPosition.dy > dy + drawHeight) {
      return null;
    }

    return Offset(
      (localPosition.dx - dx) / drawWidth,
      (localPosition.dy - dy) / drawHeight,
    );
  }

  void _onPointerDown(PointerDownEvent event) {
    if (_image == null) return;
    
    final RenderBox? box = _imageKey.currentContext?.findRenderObject() as RenderBox?;
    if (box == null) return;
    
    final localPos = box.globalToLocal(event.position);
    final normalized = _normalizePoint(localPos, box.size, Size(_image!.width.toDouble(), _image!.height.toDouble()));
    
    if (normalized != null) {
      // Handle pressure: if 0 or 1 (mouse/touch), default to 0.5. If stylus, use reported pressure.
      double pressure = event.pressure;
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

  void _onPointerMove(PointerMoveEvent event) {
    if (_currentStroke == null) return;
    
    final RenderBox? box = _imageKey.currentContext?.findRenderObject() as RenderBox?;
    if (box == null) return;
    
    final localPos = box.globalToLocal(event.position);
    final normalized = _normalizePoint(localPos, box.size, Size(_image!.width.toDouble(), _image!.height.toDouble()));
    
    if (normalized != null) {
      double pressure = event.pressure;
      if (pressure <= 0 || pressure >= 1.0) pressure = 0.5;

      setState(() {
        _currentStroke!.points.add(StrokePoint(normalized, pressure));
      });
    }
  }

  void _onPointerUp(PointerUpEvent event) {
    _currentStroke = null;
  }

  void _onPointerCancel(PointerCancelEvent event) {
    _currentStroke = null;
  }

  Future<void> _saveImage() async {
    if (_image == null) return;

    final recorder = ui.PictureRecorder();
    final canvas = Canvas(recorder);
    
    // Draw original image
    canvas.drawImage(_image!, Offset.zero, Paint());

    // Save layer for strokes to allow eraser to work properly (clearing strokes but not image)
    canvas.saveLayer(Rect.fromLTWH(0, 0, _image!.width.toDouble(), _image!.height.toDouble()), Paint());

    // Draw strokes
    final paint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;

    for (final stroke in _strokes) {
      if (stroke.points.length < 2) continue;
      
      paint.color = stroke.color;
      paint.blendMode = stroke.isEraser ? BlendMode.clear : BlendMode.srcOver;
      
      // We need to reconstruct the path
      // Points are normalized 0..1
      
      for (int i = 0; i < stroke.points.length - 1; i++) {
        final p1 = stroke.points[i];
        final p2 = stroke.points[i+1];
        
        // Simple line segment for now. 
        // For variable width (pressure), we would need to draw filled polygons or multiple lines.
        // Flutter's drawPath doesn't support variable width along the path easily.
        // We can simulate it by drawing circles or short lines with varying width.
        
        // Let's use a simple average pressure for the segment for now to keep it performant
        double pressure = (p1.pressure + p2.pressure) / 2;
        // Default pressure is 0.0 or 1.0 depending on device. 
        // If pressure is 0 (e.g. mouse), treat as 0.5 or 1.0
        if (pressure == 0) pressure = 0.5;
        if (pressure > 1.0) pressure = 1.0; // Clamp
        
        paint.strokeWidth = stroke.width * (pressure * 2) * (_image!.width / 1000.0); // Scale relative to image width

        canvas.drawLine(
          Offset(p1.point.dx * _image!.width, p1.point.dy * _image!.height),
          Offset(p2.point.dx * _image!.width, p2.point.dy * _image!.height),
          paint,
        );
      }
    }
    
    canvas.restore();

    final picture = recorder.endRecording();
    final img = await picture.toImage(_image!.width, _image!.height);
    final byteData = await img.toByteData(format: ui.ImageByteFormat.png);
    
    if (byteData != null && mounted) {
      Navigator.pop(context, byteData.buffer.asUint8List());
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
          IconButton(
            icon: const Icon(Icons.undo),
            onPressed: _strokes.isNotEmpty 
              ? () => setState(() => _strokes.removeLast()) 
              : null,
          ),
          IconButton(
            icon: const Icon(Icons.save),
            onPressed: _saveImage,
          ),
        ],
      ),
      body: Column(
        children: [
          Expanded(
            child: _image == null
                ? const Center(child: CircularProgressIndicator())
                : Listener(
                    behavior: HitTestBehavior.opaque,
                    onPointerDown: _onPointerDown,
                    onPointerMove: _onPointerMove,
                    onPointerUp: _onPointerUp,
                    onPointerCancel: _onPointerCancel,
                    child: SizedBox(
                      key: _imageKey,
                      width: double.infinity,
                      height: double.infinity,
                      child: CustomPaint(
                        painter: _EditorPainter(_image!, _strokes),
                      ),
                    ),
                  ),
          ),
          Container(
            height: 80,
            color: Colors.grey[900],
            padding: const EdgeInsets.all(8),
            child: Row(
              children: [
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
                      // Eraser Button
                      GestureDetector(
                        onTap: () => setState(() => _isEraserMode = !_isEraserMode),
                        child: Container(
                          margin: const EdgeInsets.all(8),
                          width: 40,
                          height: 40,
                          decoration: BoxDecoration(
                            color: _isEraserMode ? Colors.white : Colors.grey[800],
                            shape: BoxShape.circle,
                            border: Border.all(color: Colors.white, width: 1),
                          ),
                          child: Icon(Icons.cleaning_services, color: _isEraserMode ? Colors.black : Colors.white, size: 20),
                        ),
                      ),
                    ],
                  ),
                ),
                const VerticalDivider(color: Colors.white24),
                SizedBox(
                  width: 200,
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: [
                      const Text('Size', style: TextStyle(color: Colors.white, fontSize: 10)),
                      Slider(
                        value: _baseStrokeWidth,
                        min: 1.0,
                        max: 20.0,
                        onChanged: (v) => setState(() => _baseStrokeWidth = v),
                      ),
                    ],
                  ),
                ),
              ],
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
        margin: const EdgeInsets.all(8),
        width: 40,
        height: 40,
        decoration: BoxDecoration(
          color: color,
          shape: BoxShape.circle,
          border: (!_isEraserMode && _selectedColor == color) ? Border.all(color: Colors.white, width: 3) : null,
        ),
      ),
    );
  }
}

class _EditorPainter extends CustomPainter {
  final ui.Image image;
  final List<Stroke> strokes;

  _EditorPainter(this.image, this.strokes);

  @override
  void paint(Canvas canvas, Size size) {
    // 1. Draw Image (Contain fit)
    final double renderAspect = size.width / size.height;
    final double imageAspect = image.width / image.height;

    double drawWidth, drawHeight, dx, dy;

    if (renderAspect > imageAspect) {
      drawHeight = size.height;
      drawWidth = drawHeight * imageAspect;
      dx = (size.width - drawWidth) / 2;
      dy = 0;
    } else {
      drawWidth = size.width;
      drawHeight = drawWidth / imageAspect;
      dx = 0;
      dy = (size.height - drawHeight) / 2;
    }
    
    final imageRect = Rect.fromLTWH(dx, dy, drawWidth, drawHeight);
    paintImage(
      canvas: canvas,
      rect: imageRect,
      image: image,
      fit: BoxFit.contain,
    );

    // 2. Draw Strokes
    // Use saveLayer to allow eraser to clear strokes without clearing the image
    canvas.saveLayer(imageRect, Paint());

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
        final p2 = stroke.points[i+1];
        
        double pressure = (p1.pressure + p2.pressure) / 2;
        if (pressure == 0) pressure = 0.5;
        if (pressure > 1.0) pressure = 1.0;

        // Scale stroke width relative to the DRAWN image size on screen
        // This ensures visual consistency while drawing
        paint.strokeWidth = stroke.width * (pressure * 2) * (drawWidth / 1000.0);

        canvas.drawLine(
          Offset(dx + p1.point.dx * drawWidth, dy + p1.point.dy * drawHeight),
          Offset(dx + p2.point.dx * drawWidth, dy + p2.point.dy * drawHeight),
          paint,
        );
      }
    }
    
    canvas.restore();
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}
