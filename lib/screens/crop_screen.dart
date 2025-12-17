import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import '../services/image_processing_service.dart';

class CropScreen extends StatefulWidget {
  final Uint8List imageBytes;

  const CropScreen({super.key, required this.imageBytes});

  @override
  State<CropScreen> createState() => _CropScreenState();
}

class _CropScreenState extends State<CropScreen> {
  ui.Image? _uiImage; // For display

  // Crop State
  List<Offset> _corners = []; // TL, TR, BR, BL
  bool _isFreeForm = false; // false = Rect, true = Perspective (4-point)

  // Render State
  Size _renderSize = Size.zero;
  Offset _imageOffset = Offset.zero;

  @override
  void initState() {
    super.initState();
    _loadImage();
  }

  Future<void> _loadImage() async {
    // Decode for display
    final codec = await ui.instantiateImageCodec(widget.imageBytes);
    final frame = await codec.getNextFrame();

    setState(() {
      _uiImage = frame.image;
    });
  }

  int _activeHandle = -1; // 0=TL, 1=TR, 2=BR, 3=BL

  void _onPanStart(DragStartDetails details) {
    if (_renderSize == Size.zero) return;

    final touch = details.localPosition;
    // We need to convert the touch position to the image's coordinate system
    // relative to the image's top-left corner on screen.
    // Convert image coordinates to normalized (0..1)
    // Unused: final normalizedTouch = ...

    // Calculate screen coordinates of the handles for hit testing
    final handleScreenCoords = _corners.map((corner) {
      return Offset(
        _imageOffset.dx + corner.dx * _renderSize.width,
        _imageOffset.dy + corner.dy * _renderSize.height,
      );
    }).toList();

    double minD = 40.0; // Hit radius
    _activeHandle = -1;

    for (int i = 0; i < handleScreenCoords.length; i++) {
      if ((touch - handleScreenCoords[i]).distance < minD) {
        _activeHandle = i;
        break;
      }
    }
  }

  void _onPanUpdate(DragUpdateDetails details) {
    if (_renderSize == Size.zero || _activeHandle == -1) return;

    // Update normalized corners
    // Convert delta to normalized
    final double dxN = details.delta.dx / _renderSize.width;
    final double dyN = details.delta.dy / _renderSize.height;

    setState(() {
      // We update the active corner
      // Perspective Crop: Allow individual movement

      Offset current = _corners[_activeHandle];
      Offset newPos = current + Offset(dxN, dyN);

      // Clamp to 0..1
      newPos = Offset(newPos.dx.clamp(0.0, 1.0), newPos.dy.clamp(0.0, 1.0));

      // TODO: Add complex constraints to prevent folding?
      // For now, allow free movement.
      _corners[_activeHandle] = newPos;

      if (!_isFreeForm) {
        _enforceRectConstraints();
      }
    });
  }

  void _onPanEnd(DragEndDetails details) {
    _activeHandle = -1;
  }

  void _enforceRectConstraints() {
    // If Rect mode, we need to make sure it stays a rect.
    // But 4 handles moving independently breaks it.
    // So if Rect mode, we assume the user is dragging a corner and we update neighbors.
    // However, we just processed the drag in _onPanUpdate for the SINGLE corner.
    // We need to re-align the others.

    final br = _corners[2];
    final tl = _corners[0];
    final tr = _corners[1];
    final bl = _corners[3];

    if (_activeHandle == 0) {
      // TL moved
      _corners[1] = Offset(tr.dx, tl.dy); // TR y = TL y
      _corners[3] = Offset(tl.dx, bl.dy); // BL x = TL x
    } else if (_activeHandle == 1) {
      // TR moved
      _corners[0] = Offset(tl.dx, tr.dy); // TL y = TR y
      _corners[2] = Offset(tr.dx, br.dy); // BR x = TR x
    } else if (_activeHandle == 2) {
      // BR moved
      _corners[1] = Offset(br.dx, tr.dy); // TR x = BR x
      _corners[3] = Offset(bl.dx, br.dy); // BL y = BR y
    } else if (_activeHandle == 3) {
      // BL moved
      _corners[0] = Offset(bl.dx, tl.dy); // TL x = BL x
      _corners[2] = Offset(br.dx, bl.dy); // BR y = BL y
    }
    // Note: This logic forces rect but might feel 'snappy' if points were drifted.
    // Since we start as rect, it stays rect unless swapped.
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(title: const Text('Crop Image'), actions: []),
      bottomNavigationBar: Container(
        color: Colors.black,
        padding: const EdgeInsets.all(16),
        child: SafeArea(
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                _isFreeForm ? "Perspective" : "Rectangular",
                style: const TextStyle(color: Colors.white),
              ),
              Switch(
                value: _isFreeForm,
                onChanged: (v) => setState(() {
                  _isFreeForm = v;
                  // If switching to Rect, enforce it?
                  if (!_isFreeForm) {
                    // Reset to bounding box of current points? Or just standard rect?
                    // Let's just reset to a nice center rect to avoid confusion
                    _corners = [
                      const Offset(0.2, 0.2),
                      const Offset(0.8, 0.2),
                      const Offset(0.8, 0.8),
                      const Offset(0.2, 0.8),
                    ];
                  }
                }),
                activeThumbColor: Colors.blue,
              ),
              IconButton(
                icon: const Icon(Icons.check, color: Colors.green, size: 32),
                onPressed: () async {
                  // Logic ...
                  Uint8List? result;
                  if (_isFreeForm) {
                    result = await ImageProcessingService.instance
                        .applyPerspectiveCrop(widget.imageBytes, _corners);
                  } else {
                    // Rect crop
                    // We need to pass the bounding box of current corners (which should be a rect)
                    // Or just use the top-left and bottom-right corners logic.
                    // Since we enforce rect, we can take min/max of current points to be safe.
                    double minX = 1.0, minY = 1.0, maxX = 0.0, maxY = 0.0;
                    for (var p in _corners) {
                      if (p.dx < minX) minX = p.dx;
                      if (p.dy < minY) minY = p.dy;
                      if (p.dx > maxX) maxX = p.dx;
                      if (p.dy > maxY) maxY = p.dy;
                    }
                    // Ensure valid values
                    if (minX < 0) minX = 0;
                    if (minY < 0) minY = 0;
                    if (maxX > 1) maxX = 1;
                    if (maxY > 1) maxY = 1;

                    result = await ImageProcessingService.instance
                        .applyPerspectiveCrop(widget.imageBytes, _corners);
                  }

                  if (!context.mounted) return;

                  if (result != null) {
                    Navigator.pop(context, result);
                  } else {
                    ScaffoldMessenger.of(context).showSnackBar(
                      const SnackBar(content: Text("Crop Failed")),
                    );
                    // Return original image if crop failed
                    Navigator.pop(context, widget.imageBytes);
                  }
                },
              ),
            ],
          ),
        ),
      ),
      body: _uiImage == null
          ? const Center(child: CircularProgressIndicator())
          : LayoutBuilder(
              builder: (context, constraints) {
                // Determine image render rect
                final double renderAspect =
                    constraints.maxWidth / constraints.maxHeight;
                final double imageAspect = _uiImage!.width / _uiImage!.height;

                double drawW, drawH, dx, dy;
                if (renderAspect > imageAspect) {
                  drawH = constraints.maxHeight;
                  drawW = drawH * imageAspect;
                  dx = (constraints.maxWidth - drawW) / 2;
                  dy = 0;
                } else {
                  drawW = constraints.maxWidth;
                  drawH = drawW / imageAspect;
                  dx = 0;
                  dy = (constraints.maxHeight - drawH) / 2;
                }

                _renderSize = Size(drawW, drawH);
                _imageOffset = Offset(dx, dy);

                // Initial rect corners (Normalized 0..1)
                if (_corners.isEmpty) {
                  _corners = [
                    const Offset(0.2, 0.2), // TL
                    const Offset(0.8, 0.2), // TR
                    const Offset(0.8, 0.8), // BR
                    const Offset(0.2, 0.8), // BL
                  ];
                }

                return GestureDetector(
                  onPanStart: _onPanStart,
                  onPanUpdate: _onPanUpdate,
                  onPanEnd: _onPanEnd,
                  child: Stack(
                    children: [
                      Positioned(
                        left: dx,
                        top: dy,
                        width: drawW,
                        height: drawH,
                        child: RawImage(image: _uiImage, fit: BoxFit.contain),
                      ),
                      // Overlay Handles
                      Positioned.fill(
                        child: CustomPaint(
                          painter: _CropPainter(
                            _corners, // Pass corners directly
                            dx,
                            dy,
                            drawW,
                            drawH,
                          ),
                        ),
                      ),
                    ],
                  ),
                );
              },
            ),
    );
  }
}

class _CropPainter extends CustomPainter {
  final List<Offset> corners; // Normalized 0..1
  final double dx, dy, w, h;
  _CropPainter(this.corners, this.dx, this.dy, this.w, this.h);

  @override
  void paint(Canvas canvas, Size size) {
    if (corners.length != 4) return;

    // Convert normalized corners to screen coordinates
    final points = corners
        .map((c) => Offset(dx + c.dx * w, dy + c.dy * h))
        .toList();

    // 1. Create Path for the quad
    final path = Path()
      ..moveTo(points[0].dx, points[0].dy)
      ..lineTo(points[1].dx, points[1].dy)
      ..lineTo(points[2].dx, points[2].dy)
      ..lineTo(points[3].dx, points[3].dy)
      ..close();

    // 2. Dim outside (Clip difference)
    // Create a path for the whole screen
    final screenPath = Path()
      ..addRect(Rect.fromLTWH(0, 0, size.width, size.height));

    // Combine: Screen - Quad
    final dimPath = Path.combine(PathOperation.difference, screenPath, path);

    canvas.drawPath(dimPath, Paint()..color = Colors.black54);

    // 3. Draw Border
    final borderPaint = Paint()
      ..color = Colors.white
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2;
    canvas.drawPath(path, borderPaint);

    // 4. Draw Corners
    final cornerPaint = Paint()
      ..color = Colors.white
      ..style = PaintingStyle.fill;

    for (final point in points) {
      canvas.drawCircle(point, 10, cornerPaint);
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => true;
}
