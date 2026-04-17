import 'package:flutter/material.dart';
import 'dart:typed_data';

import '../services/filters_service.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

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
  final Function(int, String, double)? onParameterChanged;

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
    this.onParameterChanged,
  });

  @override
  State<FilterSidebar> createState() => _FilterSidebarState();
}

String _getLocalizedFilterName(BuildContext context, String nameKey) {
  final l10n = AppLocalizations.of(context)!;
  switch (nameKey) {
    case 'filterShakingStabilization':
      return l10n.filterShakingStabilization;
    case 'filterLightStabilization':
      return l10n.filterLightStabilization;
    case 'filterCornerSmoothing':
      return l10n.filterCornerSmoothing;
    case 'filterSharpening':
      return l10n.filterSharpening;
    case 'filterContrastBoost':
      return l10n.filterContrastBoost;
    case 'filterMovingAverage':
      return l10n.filterMovingAverage;
    case 'filterSmartObstacleRemoval':
      return l10n.filterSmartObstacleRemoval;
    case 'filterSmartWhiteboard':
      return l10n.filterSmartWhiteboard;
    case 'filterPersonRemoval':
      return l10n.filterPersonRemoval;
    case 'filterBlurLegacy':
      return l10n.filterBlurLegacy;
    case 'filterInvertColors':
      return l10n.filterInvertColors;
    case 'filterWhiteboardLegacy':
      return l10n.filterWhiteboardLegacy;
    case 'filterGroupStabilizers':
      return l10n.filterGroupStabilizers;
    case 'filterGroupWhiteboard':
      return l10n.filterGroupWhiteboard;
    default:
      return nameKey; // Fallback to the key if not found
  }
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
                isEditing
                    ? AppLocalizations.of(context)!.editFilterGroup
                    : AppLocalizations.of(context)!.addFilterGroup,
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
                      decoration: InputDecoration(
                        labelText: AppLocalizations.of(context)!.groupName,
                        labelStyle: const TextStyle(color: Colors.white70),
                        enabledBorder: UnderlineInputBorder(
                          borderSide: BorderSide(color: Colors.white70),
                        ),
                        focusedBorder: UnderlineInputBorder(
                          borderSide: BorderSide(color: Colors.blue),
                        ),
                      ),
                    ),
                    const SizedBox(height: 16),
                    Text(
                      AppLocalizations.of(context)!.selectFilters,
                      style: const TextStyle(
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
                              _getLocalizedFilterName(context, filter.name),
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
                    child: Text(
                      AppLocalizations.of(context)!.delete,
                      style: const TextStyle(color: Colors.red),
                    ),
                  ),
                TextButton(
                  onPressed: () => Navigator.pop(context),
                  child: Text(AppLocalizations.of(context)!.cancel),
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
                  child: Text(AppLocalizations.of(context)!.save),
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
    return Directionality(
      textDirection: TextDirection.ltr,
      child: AnimatedPositioned(
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
                        tooltip: AppLocalizations.of(context)!.allFilters,
                        onPressed: () {
                          setState(() {
                            _showGroups = false;
                          });
                        },
                      ),
                      Text(
                        AppLocalizations.of(context)!.filterGroups,
                        style: const TextStyle(
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
                              _getLocalizedFilterName(context, group.name),
                              style: const TextStyle(color: Colors.white),
                            ),
                            subtitle: Text(
                              AppLocalizations.of(
                                context,
                              )!.activeFiltersCount(group.filterIds.length),
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
                      label: Text(AppLocalizations.of(context)!.addFilterGroup),
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
                        tooltip: AppLocalizations.of(context)!.filterGroups,
                        onPressed: () {
                          setState(() {
                            _showGroups = true;
                          });
                        },
                      ),
                      Text(
                        AppLocalizations.of(context)!.allFilters,
                        style: const TextStyle(
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
                  Text(
                    AppLocalizations.of(context)!.dragToReorder,
                    style: const TextStyle(color: Colors.white54, fontSize: 12),
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
                            child: Column(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                SwitchListTile(
                                  title: Text(
                                    _getLocalizedFilterName(
                                      context,
                                      widget.filters[index].name,
                                    ),
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
                                // Show parameter slider if filter is active and has parameters
                                if (widget.filters[index].isActive &&
                                    widget
                                        .filters[index]
                                        .parameters
                                        .isNotEmpty &&
                                    widget.onParameterChanged != null)
                                  Padding(
                                    padding: const EdgeInsets.symmetric(
                                      horizontal: 16.0,
                                      vertical: 4.0,
                                    ),
                                    child: Row(
                                      children: [
                                        Text(
                                          AppLocalizations.of(
                                            context,
                                          )!.filterThresholdSensitivity,
                                          style: const TextStyle(
                                            color: Colors.white70,
                                            fontSize: 12,
                                          ),
                                        ),
                                        Expanded(
                                          child: Slider(
                                            value:
                                                widget
                                                    .filters[index]
                                                    .parameters['threshold'] ??
                                                15.0,
                                            min: 2,
                                            max: 30,
                                            divisions: 28,
                                            label:
                                                '${(widget.filters[index].parameters['threshold'] ?? 15.0).toInt()}',
                                            onChanged: (v) =>
                                                widget.onParameterChanged!(
                                                  widget.filters[index].id,
                                                  'threshold',
                                                  v,
                                                ),
                                          ),
                                        ),
                                      ],
                                    ),
                                  ),
                              ],
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
    return Directionality(
      textDirection: TextDirection.ltr,
      child: AnimatedPositioned(
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
                        AppLocalizations.of(context)!.capturedImages,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
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
                            tooltip: AppLocalizations.of(context)!.crop,
                            onPressed: () => onCropImage(currentIndex),
                          ),
                          IconButton(
                            icon: const Icon(Icons.layers, color: Colors.white),
                            tooltip: AppLocalizations.of(context)!.overlay,
                            onPressed: () => onUseAsOverlay(currentIndex),
                          ),
                          IconButton(
                            icon: const Icon(Icons.edit, color: Colors.white),
                            tooltip: AppLocalizations.of(context)!.edit,
                            onPressed: onOpenEditor,
                          ),
                          IconButton(
                            icon: const Icon(Icons.delete, color: Colors.red),
                            tooltip: AppLocalizations.of(context)!.delete,
                            onPressed: () => onDeleteImage(currentIndex),
                          ),
                        ],
                        IconButton(
                          icon: Icon(
                            isFullScreen ? Icons.fullscreen_exit : Icons.close,
                            color: Colors.white,
                          ),
                          tooltip: isFullScreen
                              ? AppLocalizations.of(context)!.fullscreenExit
                              : AppLocalizations.of(context)!.close,
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
                      ? Center(
                          child: Text(
                            AppLocalizations.of(context)!.noImagesCaptured,
                            style: const TextStyle(color: Colors.white70),
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
                                  AppLocalizations.of(
                                    context,
                                  )!.imageIndex(index + 1),
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
                                          tooltip: AppLocalizations.of(
                                            context,
                                          )!.crop,
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
                                          tooltip: AppLocalizations.of(
                                            context,
                                          )!.overlay,
                                          onPressed: () =>
                                              onUseAsOverlay(index),
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
                                          tooltip: AppLocalizations.of(
                                            context,
                                          )!.delete,
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
                  Text(
                    AppLocalizations.of(context)!.exportToPdf,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 16,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  const SizedBox(height: 8),
                  TextField(
                    controller: pdfNameController,
                    style: const TextStyle(color: Colors.white),
                    decoration: InputDecoration(
                      labelText: AppLocalizations.of(context)!.fileName,
                      labelStyle: const TextStyle(color: Colors.white70),
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
                          decoration: InputDecoration(
                            labelText: AppLocalizations.of(
                              context,
                            )!.saveLocation,
                            labelStyle: const TextStyle(color: Colors.white70),
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
                      label: Text(
                        AppLocalizations.of(context)!.exportPdfButton,
                      ),
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
      ),
    );
  }
}
