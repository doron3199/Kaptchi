import 'dart:io';
import 'dart:typed_data';
import 'package:pdf/pdf.dart';
import 'package:pdf/widgets.dart' as pw;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

class DocumentService {
  static final DocumentService instance = DocumentService._();
  DocumentService._();

  static const String _lastPdfPathKey = 'last_pdf_path';

  Future<String> getInitialPdfPath() async {
    final prefs = await SharedPreferences.getInstance();
    final String? lastPath = prefs.getString(_lastPdfPathKey);

    if (lastPath != null) {
      return lastPath;
    } else {
      final dir = await getApplicationDocumentsDirectory();
      return dir.path;
    }
  }

  Future<void> saveLastPdfPath(String path) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_lastPdfPathKey, path);
  }

  Future<File> exportPdf({
    required List<({Uint8List bytes, int width, int height})> images,
    required String fileName,
    required String directoryPath,
  }) async {
    if (images.isEmpty) {
      throw Exception('No images to export');
    }

    final pdf = pw.Document();

    for (final item in images) {
      final image = pw.MemoryImage(item.bytes);
      pdf.addPage(
        pw.Page(
          pageFormat: PdfPageFormat(
            item.width.toDouble(),
            item.height.toDouble(),
          ),
          margin: pw.EdgeInsets.zero,
          build: (pw.Context context) {
            return pw.Image(image, fit: pw.BoxFit.cover);
          },
        ),
      );
    }

    String finalFileName = fileName.trim();
    if (finalFileName.isEmpty) {
      final now = DateTime.now();
      finalFileName =
          "Capture_${now.year}-${now.month.toString().padLeft(2, '0')}-${now.day.toString().padLeft(2, '0')}_${now.hour.toString().padLeft(2, '0')}-${now.minute.toString().padLeft(2, '0')}";
    }
    if (!finalFileName.toLowerCase().endsWith('.pdf')) {
      finalFileName += '.pdf';
    }

    String finalDirPath = directoryPath.trim();
    if (finalDirPath.isEmpty) {
      final dir = await getApplicationDocumentsDirectory();
      finalDirPath = dir.path;
    }

    final file = File("$finalDirPath/$finalFileName");
    await file.writeAsBytes(await pdf.save());

    // Auto-save this path as the new default
    await saveLastPdfPath(finalDirPath);

    return file;
  }
}
