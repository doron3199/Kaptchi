import 'dart:async';
import 'package:flutter/foundation.dart';

/// Ensures the camera is owned by only one client at a time with a
/// small cool-down between releases and acquisitions.
class CameraOwnerCoordinator {
  CameraOwnerCoordinator._internal();
  static final CameraOwnerCoordinator instance = CameraOwnerCoordinator._internal();

  final Duration _cooldown = const Duration(milliseconds: 500);
  String? _currentOwner;
  DateTime? _lastRelease;

  /// Waits until no owner holds the camera and the cool-down has elapsed,
  /// then marks [owner] as the current holder.
  Future<void> acquire(String owner) async {
    while (true) {
      final now = DateTime.now();
      final elapsed = _lastRelease == null ? _cooldown : now.difference(_lastRelease!);
      final available = _currentOwner == null && elapsed >= _cooldown;
      if (available) {
        _currentOwner = owner;
        return;
      }
      await Future.delayed(const Duration(milliseconds: 50));
    }
  }

  /// Releases the camera for the given [owner] and starts the cool-down window.
  void release(String owner) {
    if (_currentOwner == owner) {
      _currentOwner = null;
      _lastRelease = DateTime.now();
    } else {
      debugPrint('CameraOwnerCoordinator: release ignored for $owner (held by $_currentOwner)');
    }
  }

  String? get currentOwner => _currentOwner;
}
