import 'dart:developer' as dev;
import 'dart:io';

/// Centralized logger for the app.
///
/// Toggle individual categories on/off to control what gets logged.
/// Output goes to both `dart:developer` log() (Debug Console) and
/// `stderr` (terminal) so logs are always visible.
///
/// Usage:
///   AppLogger.graphDebug('some message');
///   AppLogger.camera('some message');
///
/// To turn a category off, set its flag to false:
///   AppLogger.enableGraphDebug = false;
class AppLogger {
  AppLogger._();

  // ── Master switch ──────────────────────────────────────────────────
  static bool enabled = true;

  // ── Per-category switches ──────────────────────────────────────────
  static bool enableGraphDebug = true;
  static bool enableCamera = false;
  static bool enableCanvas = false;
  static bool enableFFI = true;
  static bool enableUI = false;
  static bool enableNetwork = false;

  // ── Category loggers ───────────────────────────────────────────────

  static void graphDebug(String message) {
    _log('GraphDebug', enableGraphDebug, message);
  }

  static void camera(String message) {
    _log('Camera', enableCamera, message);
  }

  static void canvas(String message) {
    _log('Canvas', enableCanvas, message);
  }

  static void ffi(String message) {
    _log('FFI', enableFFI, message);
  }

  static void ui(String message) {
    _log('UI', enableUI, message);
  }

  static void network(String message) {
    _log('Network', enableNetwork, message);
  }

  // ── Internal ───────────────────────────────────────────────────────

  static void _log(String category, bool categoryEnabled, String message) {
    if (!enabled || !categoryEnabled) return;
    final stamp = DateTime.now().toIso8601String().substring(11, 23);
    final line = '[$stamp][$category] $message';
    dev.log(line, name: 'Kaptchi');
    stderr.writeln(line);
  }
}
