import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class FilterItem {
  final int id;
  final String name;
  bool isActive;
  Map<String, double> parameters;

  FilterItem({
    required this.id,
    required this.name,
    this.isActive = false,
    Map<String, double>? parameters,
  }) : parameters = parameters ?? {};

  Map<String, dynamic> toJson() => {
    'id': id,
    'name': name,
    'isActive': isActive,
    'parameters': parameters,
  };

  factory FilterItem.fromJson(Map<String, dynamic> json) {
    return FilterItem(
      id: json['id'],
      name: json['name'],
      isActive: json['isActive'],
      parameters: json['parameters'] != null
          ? Map<String, double>.from(json['parameters'])
          : {},
    );
  }
}

class FilterGroup {
  String id;
  String name;
  List<int> filterIds;
  bool isActive;

  FilterGroup({
    required this.id,
    required this.name,
    required this.filterIds,
    this.isActive = false,
  });

  Map<String, dynamic> toJson() => {
    'id': id,
    'name': name,
    'filterIds': filterIds,
    'isActive': isActive,
  };

  factory FilterGroup.fromJson(Map<String, dynamic> json) {
    return FilterGroup(
      id: json['id'],
      name: json['name'],
      filterIds: List<int>.from(json['filterIds']),
      isActive: json['isActive'],
    );
  }
}

class FiltersService extends ChangeNotifier {
  static final FiltersService instance = FiltersService._();
  FiltersService._();

  final List<FilterItem> _filters = [
    FilterItem(id: 12, name: 'filterShakingStabilization', isActive: false),
    FilterItem(id: 13, name: 'filterLightStabilization', isActive: false),
    FilterItem(id: 14, name: 'filterCornerSmoothing', isActive: false),
    FilterItem(id: 8, name: 'filterSharpening', isActive: false),
    FilterItem(id: 7, name: 'filterContrastBoost', isActive: false),
    FilterItem(id: 6, name: 'filterMovingAverage', isActive: false),
    FilterItem(id: 5, name: 'filterSmartObstacleRemoval', isActive: false),
    FilterItem(id: 4, name: 'filterSmartWhiteboard', isActive: false),
    FilterItem(id: 11, name: 'filterPersonRemoval', isActive: false),
    FilterItem(id: 3, name: 'filterBlurLegacy', isActive: false),
    FilterItem(id: 1, name: 'filterInvertColors', isActive: false),
    FilterItem(
      id: 2,
      name: 'filterWhiteboardLegacy',
      isActive: false,
      parameters: {'threshold': 15.0},
    ),
  ];

  final List<FilterGroup> _filterGroups = [
    FilterGroup(
      id: 'stabilizers',
      name: 'filterGroupStabilizers',
      filterIds: [12, 13],
    ),
    FilterGroup(
      id: 'whiteboard',
      name: 'filterGroupWhiteboard',
      filterIds: [6, 8, 4, 11, 1],
    ),
  ];

  List<FilterItem> get filters => _filters;
  List<FilterGroup> get filterGroups => _filterGroups;

  Future<void> loadFilters() async {
    final prefs = await SharedPreferences.getInstance();

    // Migration: Check if we need to clear old format
    final String? filtersJson = prefs.getString('saved_filters');
    if (filtersJson != null) {
      final List<dynamic> decoded = jsonDecode(filtersJson);

      // Build a map of saved filter order by ID
      final Map<int, int> savedOrder = {};
      for (int i = 0; i < decoded.length; i++) {
        savedOrder[decoded[i]['id'] as int] = i;
      }

      // Reorder _filters according to saved order while keeping current names (translation keys)
      _filters.sort((a, b) {
        final orderA = savedOrder[a.id] ?? 999;
        final orderB = savedOrder[b.id] ?? 999;
        return orderA.compareTo(orderB);
      });

      // Note: isActive is always set to false on app start
      for (var f in _filters) {
        f.isActive = false;
      }
    }

    final String? groupsJson = prefs.getString('saved_filter_groups');
    if (groupsJson != null) {
      final List<dynamic> decoded = jsonDecode(groupsJson);
      final List<FilterGroup> loadedGroups = decoded
          .map((item) => FilterGroup.fromJson(item))
          .toList();

      // For groups, we need to preserve user-created groups but use translation keys for built-in ones
      // Check if the group has a built-in ID and update its name to the translation key
      for (var g in loadedGroups) {
        g.isActive = false;
        if (g.id == 'stabilizers') {
          g.name = 'filterGroupStabilizers';
        } else if (g.id == 'whiteboard') {
          g.name = 'filterGroupWhiteboard';
        }
        // User-created groups keep their original names
      }

      _filterGroups.clear();
      _filterGroups.addAll(loadedGroups);
    }
    notifyListeners();
  }

  Future<void> saveFilters() async {
    final prefs = await SharedPreferences.getInstance();

    final String filtersJson = jsonEncode(
      _filters.map((f) => f.toJson()).toList(),
    );
    await prefs.setString('saved_filters', filtersJson);

    final String groupsJson = jsonEncode(
      _filterGroups.map((g) => g.toJson()).toList(),
    );
    await prefs.setString('saved_filter_groups', groupsJson);
  }

  void toggleFilter(int index, bool isActive) {
    if (index >= 0 && index < _filters.length) {
      _filters[index].isActive = isActive;
      saveFilters();
      notifyListeners();
    }
  }

  void reorderFilters(int oldIndex, int newIndex) {
    if (oldIndex < newIndex) {
      newIndex -= 1;
    }
    final item = _filters.removeAt(oldIndex);
    _filters.insert(newIndex, item);
    saveFilters();
    notifyListeners();
  }

  void toggleGroup(String groupId, bool isActive) {
    final groupIndex = _filterGroups.indexWhere((g) => g.id == groupId);
    if (groupIndex != -1) {
      _filterGroups[groupIndex].isActive = isActive;

      for (var filterId in _filterGroups[groupIndex].filterIds) {
        final filterIndex = _filters.indexWhere((f) => f.id == filterId);
        if (filterIndex != -1) {
          _filters[filterIndex].isActive = isActive;
        }
      }
      saveFilters();
      notifyListeners();
    }
  }

  void addGroup(FilterGroup group) {
    _filterGroups.add(group);
    saveFilters();
    notifyListeners();
  }

  void editGroup(String groupId, FilterGroup newGroup) {
    final index = _filterGroups.indexWhere((g) => g.id == groupId);
    if (index != -1) {
      _filterGroups[index] = newGroup;
      saveFilters();
      notifyListeners();
    }
  }

  void deleteGroup(String groupId) {
    _filterGroups.removeWhere((g) => g.id == groupId);
    saveFilters();
    notifyListeners();
  }

  List<int> getActiveFilterIds() {
    return _filters.where((f) => f.isActive).map((f) => f.id).toList();
  }

  /// Update a parameter for a specific filter by ID
  void updateParameter(int filterId, String key, double value) {
    final filterIndex = _filters.indexWhere((f) => f.id == filterId);
    if (filterIndex != -1) {
      _filters[filterIndex].parameters[key] = value;
      saveFilters();
      notifyListeners();
    }
  }

  /// Get a filter by ID
  FilterItem? getFilterById(int id) {
    final index = _filters.indexWhere((f) => f.id == id);
    return index != -1 ? _filters[index] : null;
  }
}
