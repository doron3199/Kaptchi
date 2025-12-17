import 'package:flutter/material.dart';
import 'dart:typed_data';

import '../services/filters_service.dart';

class FilterSidebar extends StatefulWidget {
  final bool isOpen;
  final VoidCallback onClose;
  final List<FilterItem> filters;
  final List<FilterGroup> filterGroups;
  final Function(int, int) onReorder;
  final Function(int, bool) onFilterToggle;
  final Function(String, bool) onGroupToggle;
  final Function(FilterGroup) onAddGroup;
  final Function(String, FilterGroup) onEditGroup;
  final Function(String) onDeleteGroup;

  const FilterSidebar({
    super.key,
    required this.isOpen,
    required this.onClose,
    required this.filters,
    required this.filterGroups,
    required this.onReorder,
    required this.onFilterToggle,
    required this.onGroupToggle,
    required this.onAddGroup,
    required this.onEditGroup,
    required this.onDeleteGroup,
  });

  @override
  State<FilterSidebar> createState() => _FilterSidebarState();
}

class _FilterSidebarState extends State<FilterSidebar> {
  bool _showGroups = true;

  void _showAddEditGroupDialog({FilterGroup? group}) {
    final isEditing = group != null;
    final nameController = TextEditingController(text: group?.name ?? '');
    // Provide a localized copy of checked filter IDs
    final Set<int> selectedFilterIds = isEditing ? group.filterIds.toSet() : {};

    showDialog(
      context: context,
      builder: (context) {
        return StatefulBuilder(
          builder: (context, setStateDialog) {
            return AlertDialog(
              backgroundColor: Colors.grey[900],
              title: Text(
                isEditing ? 'Edit Filter Group' : 'Add Filter Group',
                style: const TextStyle(color: Colors.white),
              ),
              content: SizedBox(
                width: double.maxFinite,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    TextField(
                      controller: nameController,
                      style: const TextStyle(color: Colors.white),
                      decoration: const InputDecoration(
                        labelText: 'Group Name',
                        labelStyle: TextStyle(color: Colors.white70),
                        enabledBorder: UnderlineInputBorder(
                          borderSide: BorderSide(color: Colors.white70),
                        ),
                        focusedBorder: UnderlineInputBorder(
                          borderSide: BorderSide(color: Colors.blue),
                        ),
                      ),
                    ),
                    const SizedBox(height: 16),
                    const Text(
                      'Select Filters:',
                      style: TextStyle(
                        color: Colors.white,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    const SizedBox(height: 8),
                    Flexible(
                      child: ListView.builder(
                        physics: const ClampingScrollPhysics(),
                        shrinkWrap: true,
                        itemCount: widget.filters.length,
                        itemBuilder: (context, index) {
                          final filter = widget.filters[index];
                          final isSelected = selectedFilterIds.contains(
                            filter.id,
                          );
                          return CheckboxListTile(
                            title: Text(
                              filter.name,
                              style: const TextStyle(color: Colors.white),
                            ),
                            value: isSelected,
                            activeColor: Colors.blue,
                            checkColor: Colors.white,
                            onChanged: (bool? value) {
                              setStateDialog(() {
                                if (value == true) {
                                  selectedFilterIds.add(filter.id);
                                } else {
                                  selectedFilterIds.remove(filter.id);
                                }
                              });
                            },
                          );
                        },
                      ),
                    ),
                  ],
                ),
              ),
              actions: [
                if (isEditing)
                  TextButton(
                    onPressed: () {
                      widget.onDeleteGroup(group.id);
                      Navigator.pop(context);
                    },
                    child: const Text(
                      'Delete',
                      style: TextStyle(color: Colors.red),
                    ),
                  ),
                TextButton(
                  onPressed: () => Navigator.pop(context),
                  child: const Text('Cancel'),
                ),
                TextButton(
                  onPressed: () {
                    if (nameController.text.isNotEmpty) {
                      final newGroup = FilterGroup(
                        id: isEditing
                            ? group.id
                            : DateTime.now().millisecondsSinceEpoch.toString(),
                        name: nameController.text,
                        filterIds: selectedFilterIds.toList(),
                        isActive: isEditing ? group.isActive : false,
                      );

                      if (isEditing) {
                        widget.onEditGroup(group.id, newGroup);
                      } else {
                        widget.onAddGroup(newGroup);
                      }
                      Navigator.pop(context);
                    }
                  },
                  child: const Text('Save'),
                ),
              ],
            );
          },
        );
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedPositioned(
      duration: const Duration(milliseconds: 300),
      top: 0,
      bottom: 0,
      left: widget.isOpen ? 0 : -300,
      width: 300,
      child: GestureDetector(
        onHorizontalDragEnd: (details) {
          if (details.primaryVelocity! < -300) {
            widget.onClose();
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
              // Header
              if (_showGroups) ...[
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    // Switch to Individual Filters (Left Button)
                    IconButton(
                      icon: const Icon(Icons.tune, color: Colors.white),
                      tooltip: 'All Filters',
                      onPressed: () {
                        setState(() {
                          _showGroups = false;
                        });
                      },
                    ),
                    const Text(
                      'Filter Groups',
                      style: TextStyle(
                        fontSize: 20,
                        fontWeight: FontWeight.bold,
                        color: Colors.white,
                      ),
                    ),
                    IconButton(
                      icon: const Icon(Icons.close, color: Colors.white),
                      onPressed: widget.onClose,
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                Expanded(
                  child: ListView.builder(
                    itemCount: widget.filterGroups.length,
                    itemBuilder: (context, index) {
                      final group = widget.filterGroups[index];
                      return Card(
                        color: Colors.grey[900],
                        child: ListTile(
                          title: Text(
                            group.name,
                            style: const TextStyle(color: Colors.white),
                          ),
                          subtitle: Text(
                            '${group.filterIds.length} active filters',
                            style: const TextStyle(color: Colors.white54),
                          ),
                          trailing: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Switch(
                                value: group.isActive,
                                onChanged: (val) {
                                  widget.onGroupToggle(group.id, val);
                                },
                              ),
                              IconButton(
                                icon: const Icon(
                                  Icons.edit,
                                  color: Colors.white54,
                                  size: 20,
                                ),
                                onPressed: () {
                                  _showAddEditGroupDialog(group: group);
                                },
                              ),
                            ],
                          ),
                        ),
                      );
                    },
                  ),
                ),
                const SizedBox(height: 10),
                SizedBox(
                  width: double.infinity,
                  child: ElevatedButton.icon(
                    onPressed: () => _showAddEditGroupDialog(),
                    icon: const Icon(Icons.add),
                    label: const Text('Add Filter Group'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.blue,
                      foregroundColor: Colors.white,
                    ),
                  ),
                ),
              ] else ...[
                // Individual Filters View
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceBetween,
                  children: [
                    IconButton(
                      icon: const Icon(Icons.arrow_back, color: Colors.white),
                      onPressed: () {
                        setState(() {
                          _showGroups = true;
                        });
                      },
                    ),
                    const Text(
                      'All Filters',
                      style: TextStyle(
                        fontSize: 20,
                        fontWeight: FontWeight.bold,
                        color: Colors.white,
                      ),
                    ),
                    IconButton(
                      icon: const Icon(Icons.close, color: Colors.white),
                      onPressed: widget.onClose,
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
                    onReorder: widget.onReorder,
                    children: [
                      for (
                        int index = 0;
                        index < widget.filters.length;
                        index++
                      )
                        Card(
                          key: ValueKey(widget.filters[index].id),
                          color: Colors.grey[900],
                          child: SwitchListTile(
                            title: Text(
                              widget.filters[index].name,
                              style: const TextStyle(color: Colors.white),
                            ),
                            value: widget.filters[index].isActive,
                            onChanged: (bool value) =>
                                widget.onFilterToggle(index, value),
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
  final int currentIndex;
  final VoidCallback onClose;
  final VoidCallback onToggleFullScreen;
  final VoidCallback onOpenEditor;
  final Function(int) onDeleteImage;
  final Function(int) onPageChanged;
  final TextEditingController pdfNameController;
  final TextEditingController pdfPathController;
  final VoidCallback onExportPdf;
  final VoidCallback onSelectDirectory;
  final Function(int) onCropImage;
  final Function(int) onUseAsOverlay;

  const GallerySidebar({
    super.key,
    required this.isOpen,
    required this.isFullScreen,
    required this.capturedImages,
    required this.currentIndex,
    required this.onClose,
    required this.onToggleFullScreen,
    required this.onOpenEditor,
    required this.onDeleteImage,
    required this.onPageChanged,
    required this.pdfNameController,
    required this.pdfPathController,
    required this.onExportPdf,
    required this.onSelectDirectory,
    required this.onCropImage,
    required this.onUseAsOverlay,
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
                  Expanded(
                    child: Text(
                      'Captured Images',
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        fontSize: 20,
                        fontWeight: FontWeight.bold,
                        color: Colors.white,
                      ),
                    ),
                  ),
                  Row(
                    children: [
                      if (isFullScreen && capturedImages.isNotEmpty) ...[
                        IconButton(
                          icon: const Icon(Icons.crop, color: Colors.white),
                          onPressed: () => onCropImage(currentIndex),
                        ),
                        IconButton(
                          icon: const Icon(Icons.layers, color: Colors.white),
                          onPressed: () => onUseAsOverlay(currentIndex),
                        ),
                        IconButton(
                          icon: const Icon(Icons.edit, color: Colors.white),
                          onPressed: onOpenEditor,
                        ),
                        IconButton(
                          icon: const Icon(Icons.delete, color: Colors.red),
                          onPressed: () => onDeleteImage(currentIndex),
                        ),
                      ],
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
                          // Wrap image in a container without InteractiveViewer
                          // to allow PageView scrolling without zoom interference
                          return Center(
                            child: Image.memory(
                              item.bytes,
                              fit: BoxFit.contain,
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
                              trailing: SizedBox(
                                width: 140,
                                child: SingleChildScrollView(
                                  scrollDirection: Axis.horizontal,
                                  child: Row(
                                    mainAxisSize: MainAxisSize.min,
                                    children: [
                                      IconButton(
                                        padding: EdgeInsets.zero,
                                        constraints: const BoxConstraints(),
                                        icon: const Icon(
                                          Icons.crop,
                                          color: Colors.white,
                                          size: 20,
                                        ),
                                        onPressed: () => onCropImage(index),
                                      ),
                                      const SizedBox(width: 8),
                                      IconButton(
                                        padding: EdgeInsets.zero,
                                        constraints: const BoxConstraints(),
                                        icon: const Icon(
                                          Icons.layers,
                                          color: Colors.white,
                                          size: 20,
                                        ),
                                        onPressed: () => onUseAsOverlay(index),
                                      ),
                                      const SizedBox(width: 8),
                                      IconButton(
                                        padding: EdgeInsets.zero,
                                        constraints: const BoxConstraints(),
                                        icon: const Icon(
                                          Icons.delete,
                                          color: Colors.redAccent,
                                          size: 20,
                                        ),
                                        onPressed: () => onDeleteImage(index),
                                      ),
                                    ],
                                  ),
                                ),
                              ),
                            ),
                          );
                        },
                      ),
              ),
              if (!isFullScreen) ...[
                const Divider(color: Colors.white24),
                const Text(
                  'Export to PDF',
                  style: TextStyle(
                    color: Colors.white,
                    fontSize: 16,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 8),
                TextField(
                  controller: pdfNameController,
                  style: const TextStyle(color: Colors.white),
                  decoration: const InputDecoration(
                    labelText: 'File Name',
                    labelStyle: TextStyle(color: Colors.white70),
                    enabledBorder: UnderlineInputBorder(
                      borderSide: BorderSide(color: Colors.white70),
                    ),
                  ),
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    Expanded(
                      child: TextField(
                        controller: pdfPathController,
                        style: const TextStyle(
                          color: Colors.white,
                          fontSize: 12,
                        ),
                        readOnly: true,
                        decoration: const InputDecoration(
                          labelText: 'Save Location',
                          labelStyle: TextStyle(color: Colors.white70),
                          enabledBorder: UnderlineInputBorder(
                            borderSide: BorderSide(color: Colors.white70),
                          ),
                        ),
                      ),
                    ),
                    IconButton(
                      icon: const Icon(Icons.folder_open, color: Colors.blue),
                      onPressed: onSelectDirectory,
                    ),
                  ],
                ),
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  child: ElevatedButton.icon(
                    onPressed: capturedImages.isNotEmpty ? onExportPdf : null,
                    icon: const Icon(Icons.picture_as_pdf),
                    label: const Text('Export PDF'),
                    style: ElevatedButton.styleFrom(
                      backgroundColor: Colors.blue,
                      foregroundColor: Colors.white,
                    ),
                  ),
                ),
                const SizedBox(height: 10),
              ],
            ],
          ),
        ),
      ),
    );
  }
}
