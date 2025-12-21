import 'package:flutter/material.dart';
import 'package:url_launcher/url_launcher.dart';
import 'package:kaptchi_flutter/l10n/app_localizations.dart';

class InfoScreen extends StatelessWidget {
  const InfoScreen({super.key});

  @override
  Widget build(BuildContext context) {
    final l10n = AppLocalizations.of(context)!;
    return Scaffold(
      appBar: AppBar(title: Text(l10n.aboutKaptchi), centerTitle: true),
      body: SingleChildScrollView(
        padding: const EdgeInsets.all(24.0),
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 800),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                _buildHeader(context),
                const SizedBox(height: 32),
                _buildSection(
                  title: l10n.whatIsKaptchi,
                  content: l10n.whatIsKaptchiDescription,
                  icon: Icons.visibility_rounded,
                ),
                const SizedBox(height: 16),
                _buildSection(
                  title: l10n.singleMonitorTip,
                  content: l10n.singleMonitorTipDescription,
                  icon: Icons.monitor_rounded,
                  child: Semantics(
                    link: true,
                    label: l10n.getVirtualDisplayDriver,
                    child: InkWell(
                      onTap: () => launchUrl(
                        Uri.parse(
                          'https://github.com/VirtualDrivers/Virtual-Display-Driver',
                        ),
                      ),
                      child: Text(
                        l10n.getVirtualDisplayDriver,
                        style: const TextStyle(
                          color: Colors.blueAccent,
                          decoration: TextDecoration.underline,
                        ),
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 16),
                _buildSection(
                  title: l10n.smartFilters,
                  content: '',
                  icon: Icons.star_outline_rounded,
                  child: Column(
                    children: [
                      _buildFeatureItem(
                        icon: Icons.cameraswitch_outlined,
                        title: l10n.multiSourceCapable,
                        description: l10n.multiSourceDescription,
                      ),
                      _buildFeatureItem(
                        icon: Icons.auto_fix_high_outlined,
                        title: l10n.smartFilters,
                        description: l10n.smartFiltersDescription,
                      ),
                      _buildFeatureItem(
                        icon: Icons.picture_as_pdf_outlined,
                        title: l10n.exportToPdf,
                        description: l10n.pdfExportDescription,
                      ),
                      _buildFeatureItem(
                        icon: Icons.wifi_tethering,
                        title: l10n.mobileConnect,
                        description: l10n.mobileConnectDescription,
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 16),
                _buildSection(
                  title: l10n.howToUse,
                  content: '',
                  icon: Icons.help_outline_rounded,
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      _buildStep(1, l10n.step1),
                      _buildStep(2, l10n.step2),
                      _buildStep(3, l10n.step3),
                      _buildStep(4, l10n.step4),
                      _buildStep(5, l10n.step5),
                    ],
                  ),
                ),
                const SizedBox(height: 32),
                _buildFooter(context, l10n),
                const SizedBox(height: 32),
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildHeader(BuildContext context) {
    return Column(
      children: [
        Container(
          padding: const EdgeInsets.all(20),
          decoration: BoxDecoration(
            color: Colors.blueAccent.withValues(alpha: 0.1),
            shape: BoxShape.circle,
          ),
          child: const Icon(
            Icons.camera_enhance_rounded,
            size: 64,
            color: Colors.blueAccent,
          ),
        ),
        const SizedBox(height: 16),
        const Text(
          'Kaptchi',
          style: TextStyle(
            fontSize: 32,
            fontWeight: FontWeight.bold,
            letterSpacing: 1.2,
          ),
        ),
        const SizedBox(height: 8),
        const Text(
          'v1.0.0',
          style: TextStyle(color: Colors.grey, fontSize: 14),
        ),
      ],
    );
  }

  Widget _buildSection({
    required String title,
    required String content,
    required IconData icon,
    Widget? child,
  }) {
    return Card(
      elevation: 2,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
      child: Padding(
        padding: const EdgeInsets.all(20.0),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Icon(icon, color: Colors.blueAccent),
                const SizedBox(width: 12),
                Text(
                  title,
                  style: const TextStyle(
                    fontSize: 20,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
            if (content.isNotEmpty) ...[
              const SizedBox(height: 12),
              Text(content, style: const TextStyle(height: 1.5, fontSize: 15)),
            ],
            if (child != null) ...[const SizedBox(height: 16), child],
          ],
        ),
      ),
    );
  }

  Widget _buildFeatureItem({
    required IconData icon,
    required String title,
    required String description,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8.0),
      child: Row(
        children: [
          Container(
            padding: const EdgeInsets.all(8),
            decoration: BoxDecoration(
              color: Colors.grey.withValues(alpha: 0.1),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Icon(icon, size: 20, color: Colors.blueGrey),
          ),
          const SizedBox(width: 16),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: const TextStyle(
                    fontWeight: FontWeight.bold,
                    fontSize: 15,
                  ),
                ),
                Text(
                  description,
                  style: const TextStyle(fontSize: 13, color: Colors.grey),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildStep(int number, String text) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6.0),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 24,
            height: 24,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: Colors.blueAccent,
              shape: BoxShape.circle,
            ),
            child: Text(
              number.toString(),
              style: const TextStyle(
                color: Colors.white,
                fontWeight: FontWeight.bold,
                fontSize: 12,
              ),
            ),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              text,
              style: const TextStyle(fontSize: 15, height: 1.4),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildFooter(BuildContext context, AppLocalizations l10n) {
    return Column(
      children: [
        Text(l10n.madeWithLove, style: const TextStyle(color: Colors.grey)),
        const SizedBox(height: 8),
        InkWell(
          onTap: () async {
            final Uri url = Uri.parse(
              'https://github.com/doron3199/kaptchi_flutter',
            );
            if (!await launchUrl(url)) {
              // ignore: use_build_context_synchronously
              ScaffoldMessenger.of(
                context,
              ).showSnackBar(SnackBar(content: Text(l10n.couldNotOpenLink)));
            }
          },
          child: Text(
            l10n.visitGithub,
            style: const TextStyle(
              color: Colors.blueAccent,
              decoration: TextDecoration.underline,
            ),
          ),
        ),
      ],
    );
  }
}
