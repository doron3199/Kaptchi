import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class FilterItem {
  final int id;
  final String name;
  bool isActive;

  FilterItem({required this.id, required this.name, this.isActive = false});

  Map<String, dynamic> toJson() => {
    'id': id,
    'name': name,
    'isActive': isActive,
  };

  factory FilterItem.fromJson(Map<String, dynamic> json) {
    return FilterItem(
      id: json['id'],
      name: json['name'],
      isActive: json['isActive'],
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
    FilterItem(id: 12, name: 'Shaking Stabilization', isActive: false),
    FilterItem(id: 13, name: 'Light Stabilization', isActive: false),
    FilterItem(id: 14, name: 'Corner Smoothing', isActive: false),
    FilterItem(id: 8, name: 'Sharpening', isActive: false),
    FilterItem(id: 7, name: 'Contrast Boost (CLAHE)', isActive: false),
    FilterItem(id: 6, name: 'Moving Average', isActive: false),
    FilterItem(id: 5, name: 'Smart Obstacle Removal', isActive: false),
    FilterItem(id: 4, name: 'Smart Whiteboard', isActive: false),
    FilterItem(id: 11, name: 'Person Removal (AI)', isActive: false),
    FilterItem(id: 3, name: 'Blur (Legacy)', isActive: false),
    FilterItem(id: 1, name: 'Invert Colors', isActive: false),
    FilterItem(id: 2, name: 'Whiteboard (Legacy)', isActive: false),
  ];

  final List<FilterGroup> _filterGroups = [
    FilterGroup(id: 'stabilizers', name: 'Stabilizers', filterIds: [12, 13]),
    FilterGroup(
      id: 'whiteboard',
      name: 'Whiteboard',
      filterIds: [6, 8, 4, 11, 1],
    ),
  ];

  List<FilterItem> get filters => _filters;
  List<FilterGroup> get filterGroups => _filterGroups;

  Future<void> loadFilters() async {
    final prefs = await SharedPreferences.getInstance();

    final String? filtersJson = prefs.getString('saved_filters');
    if (filtersJson != null) {
      final List<dynamic> decoded = jsonDecode(filtersJson);
      final List<FilterItem> loadedFilters = decoded
          .map((item) => FilterItem.fromJson(item))
          .toList();

      // Ensure filters are off on app start
      for (var f in loadedFilters) {
        f.isActive = false;
      }

      _filters.clear();
      _filters.addAll(loadedFilters);
    }

    final String? groupsJson = prefs.getString('saved_filter_groups');
    if (groupsJson != null) {
      final List<dynamic> decoded = jsonDecode(groupsJson);
      final List<FilterGroup> loadedGroups = decoded
          .map((item) => FilterGroup.fromJson(item))
          .toList();

      // Ensure groups are off on app start
      for (var g in loadedGroups) {
        g.isActive = false;
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
}
