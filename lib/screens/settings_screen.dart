import 'package:flutter/material.dart';
import '../services/language_service.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context);

    // Fallback if l10n is null (shouldn't happen if properly hooked up in main)
    if (l10n == null) return const SizedBox.shrink();

    return Scaffold(
      appBar: AppBar(title: Text(l10n.settings)),
      body: ListView(
        children: [
          ValueListenableBuilder<Locale>(
            valueListenable: LanguageService.instance.localeNotifier,
            builder: (context, currentLocale, child) {
              return ListTile(
                title: Text(l10n.language),
                trailing: DropdownButton<Locale>(
                  value: currentLocale,
                  items: [
                    DropdownMenuItem(
                      value: const Locale('en'),
                      child: Text(l10n.english),
                    ),
                    DropdownMenuItem(
                      value: const Locale('he'),
                      child: Text(l10n.hebrew),
                    ),
                  ],
                  onChanged: (Locale? newLocale) {
                    if (newLocale != null) {
                      LanguageService.instance.changeLanguage(newLocale);
                    }
                  },
                ),
              );
            },
          ),
        ],
      ),
    );
  }
}
