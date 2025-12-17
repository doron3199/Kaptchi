import 'package:flutter/foundation.dart';

class GalleryService extends ChangeNotifier {
  static final GalleryService instance = GalleryService._();
  GalleryService._();

  final List<({Uint8List bytes, int width, int height})> _images = [];

  List<({Uint8List bytes, int width, int height})> get images =>
      List.unmodifiable(_images);

  void addImage(Uint8List bytes, int width, int height) {
    _images.add((bytes: bytes, width: width, height: height));
    notifyListeners();
  }

  void removeImage(int index) {
    if (index >= 0 && index < _images.length) {
      _images.removeAt(index);
      notifyListeners();
    }
  }

  void replaceImage(int index, Uint8List bytes, int width, int height) {
    if (index >= 0 && index < _images.length) {
      _images[index] = (bytes: bytes, width: width, height: height);
      notifyListeners();
    }
  }

  // Shared Export Configuration

  void clear() {
    _images.clear();
    notifyListeners();
  }
}
