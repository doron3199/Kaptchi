import 'package:flutter/material.dart';
import 'dart:typed_data';

class FilterItem {
  final int id;
  final String name;
  bool isActive;

  FilterItem({required this.id, required this.name, this.isActive = false});
}

class FilterSidebar extends StatelessWidget {
  final bool isOpen;
  final VoidCallback onClose;
  final List<FilterItem> filters;
  final Function(int, int) onReorder;
  final Function(int, bool) onFilterToggle;

  const FilterSidebar({
    super.key,
    required this.isOpen,
    required this.onClose,
    required this.filters,
    required this.onReorder,
    required this.onFilterToggle,
  });

  @override
  Widget build(BuildContext context) {
    return AnimatedPositioned(
      duration: const Duration(milliseconds: 300),
      top: 0,
      bottom: 0,
      left: isOpen ? 0 : -300,
      width: 300,
      child: GestureDetector(
        onHorizontalDragEnd: (details) {
          if (details.primaryVelocity! < -300) {
            onClose();
          }
        },
        child: Container(
          decoration: const BoxDecoration(
            color: Colors.black,
            boxShadow: [
              BoxShadow(
                color: Color(0x80000000),
                blurRadius: 5,
                offset: Offset(2, 0),
              ),
            ],
          ),
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Text(
                    'Filters',
                    style: TextStyle(
                      fontSize: 20,
                      fontWeight: FontWeight.bold,
                      color: Colors.white,
                    ),
                  ),
                  IconButton(
                    icon: const Icon(Icons.close, color: Colors.white),
                    onPressed: onClose,
                  ),
                ],
              ),
              const SizedBox(height: 10),
              const Text(
                'Drag to reorder. Top filters apply first.',
                style: TextStyle(color: Colors.white54, fontSize: 12),
              ),
              const SizedBox(height: 10),
              Expanded(
                child: ReorderableListView(
                  onReorder: onReorder,
                  children: [
                    for (int index = 0; index < filters.length; index++)
                      Card(
                        key: ValueKey(filters[index].id),
                        color: Colors.grey[900],
                        child: SwitchListTile(
                          title: Text(
                            filters[index].name,
                            style: const TextStyle(color: Colors.white),
                          ),
                          value: filters[index].isActive,
                          onChanged: (bool value) =>
                              onFilterToggle(index, value),
                          secondary: const Icon(
                            Icons.drag_handle,
                            color: Colors.white54,
                          ),
                        ),
                      ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class GallerySidebar extends StatelessWidget {
  final bool isOpen;
  final bool isFullScreen;
  final List<({Uint8List bytes, int width, int height})> capturedImages;
  final VoidCallback onClose;
  final VoidCallback onToggleFullScreen;
  final VoidCallback onOpenEditor;
  final Function(int) onDeleteImage;
  final Function(int) onPageChanged;
  final TextEditingController pdfNameController;
  final TextEditingController pdfPathController;
  final VoidCallback onExportPdf;
  final VoidCallback onSelectDirectory;

  const GallerySidebar({
    super.key,
    required this.isOpen,
    required this.isFullScreen,
    required this.capturedImages,
    required this.onClose,
    required this.onToggleFullScreen,
    required this.onOpenEditor,
    required this.onDeleteImage,
    required this.onPageChanged,
    required this.pdfNameController,
    required this.pdfPathController,
    required this.onExportPdf,
    required this.onSelectDirectory,
  });

  @override
  Widget build(BuildContext context) {
    return AnimatedPositioned(
      duration: const Duration(milliseconds: 300),
      top: 0,
      bottom: 0,
      right: isOpen
          ? 0
          : (isFullScreen ? -MediaQuery.of(context).size.width : -350),
      width: isFullScreen ? MediaQuery.of(context).size.width : 350,
      child: GestureDetector(
        onHorizontalDragEnd: (details) {
          if (details.primaryVelocity! < -300) {
            // Swipe Left
            if (!isFullScreen) {
              onToggleFullScreen();
            }
          } else if (details.primaryVelocity! > 300) {
            // Swipe Right
            if (isFullScreen) {
              onToggleFullScreen();
            } else {
              onClose();
            }
          }
        },
        child: Container(
          decoration: const BoxDecoration(
            color: Colors.black,
            boxShadow: [
              BoxShadow(
                color: Color(0x80000000),
                blurRadius: 5,
                offset: Offset(-2, 0),
              ),
            ],
          ),
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Text(
                    'Captured Images',
                    style: TextStyle(
                      fontSize: 20,
                      fontWeight: FontWeight.bold,
                      color: Colors.white,
                    ),
                  ),
                  Row(
                    children: [
                      if (isFullScreen && capturedImages.isNotEmpty)
                        IconButton(
                          icon: const Icon(Icons.edit, color: Colors.white),
                          onPressed: onOpenEditor,
                        ),
                      IconButton(
                        icon: Icon(
                          isFullScreen ? Icons.fullscreen_exit : Icons.close,
                          color: Colors.white,
                        ),
                        onPressed: () {
                          if (isFullScreen) {
                            onToggleFullScreen();
                          } else {
                            onClose();
                          }
                        },
                      ),
                    ],
                  ),
                ],
              ),
              const SizedBox(height: 10),
              Expanded(
                child: capturedImages.isEmpty
                    ? const Center(
                        child: Text(
                          'No images captured',
                          style: TextStyle(color: Colors.white70),
                        ),
                      )
                    : isFullScreen
                    ? PageView.builder(
                        scrollDirection: Axis.vertical,
                        itemCount: capturedImages.length,
                        onPageChanged: onPageChanged,
                        itemBuilder: (context, index) {
                          final item = capturedImages[index];
                          return InteractiveViewer(
                            minScale: 1.0,
                            maxScale: 5.0,
                            child: Center(
                              child: Image.memory(
                                item.bytes,
                                fit: BoxFit.contain,
                              ),
                            ),
                          );
                        },
                      )
                    : ListView.builder(
                        itemCount: capturedImages.length,
                        itemBuilder: (context, index) {
                          final item = capturedImages[index];
                          return Card(
                            color: Colors.blue[900],
                            margin: const EdgeInsets.only(bottom: 8),
                            child: ListTile(
                              contentPadding: const EdgeInsets.all(8),
                              leading: Image.memory(
                                item.bytes,
                                width: 60,
                                height: 60,
                                fit: BoxFit.cover,
                              ),
                              title: Text(
                                'Image ${index + 1}',
                                style: const TextStyle(color: Colors.white),
                              ),
                              trailing: IconButton(
                                icon: const Icon(
                                  Icons.delete,
                                  color: Colors.redAccent,
                                ),
                                onPressed: () => onDeleteImage(index),
                              ),
                            ),
                          );
                        },
                      ),
              ),
              if (!isFullScreen) ...[
                const Divider(thickness: 1, color: Colors.white24),
                const Text(
                  'PDF Settings',
                  style: TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                    color: Colors.white,
                  ),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: pdfNameController,
                  style: const TextStyle(color: Colors.white),
                  decoration: const InputDecoration(
                    labelText: 'File Name',
                    labelStyle: TextStyle(color: Colors.white70),
                    hintText: 'Default: Capture_YYYY-MM-DD_HH-MM',
                    hintStyle: TextStyle(color: Colors.white30),
                    enabledBorder: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white30),
                    ),
                    focusedBorder: OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.blue),
                    ),
                    isDense: true,
                  ),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: pdfPathController,
                  style: const TextStyle(color: Colors.white),
                  decoration: InputDecoration(
                    labelText: 'Save Directory',
                    labelStyle: const TextStyle(color: Colors.white70),
                    enabledBorder: const OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.white30),
                    ),
                    focusedBorder: const OutlineInputBorder(
                      borderSide: BorderSide(color: Colors.blue),
                    ),
                    isDense: true,
                    suffixIcon: IconButton(
                      icon: const Icon(Icons.more_horiz, color: Colors.white),
                      onPressed: onSelectDirectory,
                    ),
                  ),
                ),
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  height: 50,
                  child: ElevatedButton.icon(
                    onPressed: capturedImages.isNotEmpty ? onExportPdf : null,
                    icon: const Icon(Icons.save_alt),
                    label: const Text('Export PDF'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.blue,
                      foregroundColor: Colors.white,
                    ),
                  ),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}
