import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/language_service.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

class SettingsScreen extends StatefulWidget {
  const SettingsScreen({super.key});

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

class _SettingsScreenState extends State<SettingsScreen> {
  bool _defaultAIMode = true;

  @override
  void initState() {
    super.initState();
    SharedPreferences.getInstance().then((prefs) {
      setState(() {
        _defaultAIMode = prefs.getBool('default_ai_mode') ?? true;
      });
    });
  }

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context);
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
          SwitchListTile(
            title: const Text('Default AI Mode'),
            subtitle: const Text('Auto-enable AI when whiteboard starts'),
            value: _defaultAIMode,
            onChanged: (bool value) async {
              final prefs = await SharedPreferences.getInstance();
              await prefs.setBool('default_ai_mode', value);
              setState(() {
                _defaultAIMode = value;
              });
            },
          ),
        ],
      ),
    );
  }
}
