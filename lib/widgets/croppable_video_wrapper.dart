import 'package:flutter/material.dart';

/// A widget that wraps a child and applies visual cropping from top and bottom.
/// Also provides draggable handles when in crop mode.
class CroppableVideoWrapper extends StatelessWidget {
  final Widget child;
  final bool isCropMode;
  final double cropTopFraction;
  final double cropBottomFraction;
  final ValueChanged<double> onCropTopChanged;
  final ValueChanged<double> onCropBottomChanged;

  const CroppableVideoWrapper({
    super.key,
    required this.child,
    required this.isCropMode,
    required this.cropTopFraction,
    required this.cropBottomFraction,
    required this.onCropTopChanged,
    required this.onCropBottomChanged,
  });

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final totalHeight = constraints.maxHeight;
        final cropTop = cropTopFraction * totalHeight;
        final cropBottom = (1.0 - cropBottomFraction) * totalHeight;

        return Stack(
          children: [
            // The cropped video content
            ClipRect(
              clipper: _TopBottomClipper(top: cropTop, bottom: cropBottom),
              child: child,
            ),

            // Darkened areas (to show what's being cropped)
            if (isCropMode) ...[
              // Top dark overlay
              Positioned(
                top: 0,
                left: 0,
                right: 0,
                height: cropTop,
                child: Container(color: Colors.black.withOpacity(0.6)),
              ),
              // Bottom dark overlay
              Positioned(
                bottom: 0,
                left: 0,
                right: 0,
                height: cropBottom,
                child: Container(color: Colors.black.withOpacity(0.6)),
              ),
              // Top handle
              Positioned(
                top: cropTop - 15,
                left: 0,
                right: 0,
                child: GestureDetector(
                  onVerticalDragUpdate: (details) {
                    final newTop = (cropTop + details.delta.dy) / totalHeight;
                    onCropTopChanged(
                      newTop.clamp(0.0, cropBottomFraction - 0.1),
                    );
                  },
                  child: Container(
                    height: 30,
                    color: Colors.transparent,
                    child: Center(
                      child: Container(
                        width: 60,
                        height: 6,
                        decoration: BoxDecoration(
                          color: Colors.white,
                          borderRadius: BorderRadius.circular(3),
                          boxShadow: [
                            BoxShadow(
                              color: Colors.black.withOpacity(0.5),
                              blurRadius: 4,
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
              ),
              // Bottom handle
              Positioned(
                top: totalHeight - cropBottom - 15,
                left: 0,
                right: 0,
                child: GestureDetector(
                  onVerticalDragUpdate: (details) {
                    final newBottom =
                        (cropBottom - details.delta.dy) / totalHeight;
                    final newBottomFraction = 1.0 - newBottom;
                    onCropBottomChanged(
                      newBottomFraction.clamp(cropTopFraction + 0.1, 1.0),
                    );
                  },
                  child: Container(
                    height: 30,
                    color: Colors.transparent,
                    child: Center(
                      child: Container(
                        width: 60,
                        height: 6,
                        decoration: BoxDecoration(
                          color: Colors.white,
                          borderRadius: BorderRadius.circular(3),
                          boxShadow: [
                            BoxShadow(
                              color: Colors.black.withOpacity(0.5),
                              blurRadius: 4,
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ],
          ],
        );
      },
    );
  }
}

class _TopBottomClipper extends CustomClipper<Rect> {
  final double top;
  final double bottom;

  _TopBottomClipper({required this.top, required this.bottom});

  @override
  Rect getClip(Size size) {
    return Rect.fromLTRB(0, top, size.width, size.height - bottom);
  }

  @override
  bool shouldReclip(_TopBottomClipper oldClipper) {
    return oldClipper.top != top || oldClipper.bottom != bottom;
  }
}
