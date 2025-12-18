import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';

class LanguageService {
  // Singleton pattern
  static final LanguageService instance = LanguageService._();
  LanguageService._();

  // Notifier for the current locale
  final ValueNotifier<Locale> localeNotifier = ValueNotifier(
    const Locale('en'),
  );

  // Initialize service and load saved language preference
  Future<void> initialize() async {
    final prefs = await SharedPreferences.getInstance();
    final String? languageCode = prefs.getString('language_code');
    if (languageCode != null) {
      localeNotifier.value = Locale(languageCode);
    }
  }

  // Change the language and save preference
  Future<void> changeLanguage(Locale locale) async {
    if (localeNotifier.value == locale) return;

    localeNotifier.value = locale;

    final prefs = await SharedPreferences.getInstance();
    await prefs.setString('language_code', locale.languageCode);
  }
}
