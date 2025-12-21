import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:intl/intl.dart' as intl;

import 'app_localizations_en.dart';
import 'app_localizations_he.dart';

// ignore_for_file: type=lint

/// Callers can lookup localized strings with an instance of AppLocalizations
/// returned by `AppLocalizations.of(context)`.
///
/// Applications need to include `AppLocalizations.delegate()` in their app's
/// `localizationDelegates` list, and the locales they support in the app's
/// `supportedLocales` list. For example:
///
/// ```dart
/// import 'l10n/app_localizations.dart';
///
/// return MaterialApp(
///   localizationsDelegates: AppLocalizations.localizationsDelegates,
///   supportedLocales: AppLocalizations.supportedLocales,
///   home: MyApplicationHome(),
/// );
/// ```
///
/// ## Update pubspec.yaml
///
/// Please make sure to update your pubspec.yaml to include the following
/// packages:
///
/// ```yaml
/// dependencies:
///   # Internationalization support.
///   flutter_localizations:
///     sdk: flutter
///   intl: any # Use the pinned version from flutter_localizations
///
///   # Rest of dependencies
/// ```
///
/// ## iOS Applications
///
/// iOS applications define key application metadata, including supported
/// locales, in an Info.plist file that is built into the application bundle.
/// To configure the locales supported by your app, you’ll need to edit this
/// file.
///
/// First, open your project’s ios/Runner.xcworkspace Xcode workspace file.
/// Then, in the Project Navigator, open the Info.plist file under the Runner
/// project’s Runner folder.
///
/// Next, select the Information Property List item, select Add Item from the
/// Editor menu, then select Localizations from the pop-up menu.
///
/// Select and expand the newly-created Localizations item then, for each
/// locale your application supports, add a new item and select the locale
/// you wish to add from the pop-up menu in the Value field. This list should
/// be consistent with the languages listed in the AppLocalizations.supportedLocales
/// property.
abstract class AppLocalizations {
  AppLocalizations(String locale)
    : localeName = intl.Intl.canonicalizedLocale(locale.toString());

  final String localeName;

  static AppLocalizations? of(BuildContext context) {
    return Localizations.of<AppLocalizations>(context, AppLocalizations);
  }

  static const LocalizationsDelegate<AppLocalizations> delegate =
      _AppLocalizationsDelegate();

  /// A list of this localizations delegate along with the default localizations
  /// delegates.
  ///
  /// Returns a list of localizations delegates containing this delegate along with
  /// GlobalMaterialLocalizations.delegate, GlobalCupertinoLocalizations.delegate,
  /// and GlobalWidgetsLocalizations.delegate.
  ///
  /// Additional delegates can be added by appending to this list in
  /// MaterialApp. This list does not have to be used at all if a custom list
  /// of delegates is preferred or required.
  static const List<LocalizationsDelegate<dynamic>> localizationsDelegates =
      <LocalizationsDelegate<dynamic>>[
        delegate,
        GlobalMaterialLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
      ];

  /// A list of this localizations delegate's supported locales.
  static const List<Locale> supportedLocales = <Locale>[
    Locale('en'),
    Locale('he'),
  ];

  /// No description provided for @appTitle.
  ///
  /// In en, this message translates to:
  /// **'Kaptchi'**
  String get appTitle;

  /// No description provided for @settings.
  ///
  /// In en, this message translates to:
  /// **'Settings'**
  String get settings;

  /// No description provided for @language.
  ///
  /// In en, this message translates to:
  /// **'Language'**
  String get language;

  /// No description provided for @english.
  ///
  /// In en, this message translates to:
  /// **'English'**
  String get english;

  /// No description provided for @hebrew.
  ///
  /// In en, this message translates to:
  /// **'Hebrew'**
  String get hebrew;

  /// No description provided for @availableCameras.
  ///
  /// In en, this message translates to:
  /// **'Available Cameras'**
  String get availableCameras;

  /// No description provided for @noCamerasFound.
  ///
  /// In en, this message translates to:
  /// **'No cameras found'**
  String get noCamerasFound;

  /// No description provided for @exportSettings.
  ///
  /// In en, this message translates to:
  /// **'Export Settings'**
  String get exportSettings;

  /// No description provided for @fileName.
  ///
  /// In en, this message translates to:
  /// **'File Name'**
  String get fileName;

  /// No description provided for @saveLocation.
  ///
  /// In en, this message translates to:
  /// **'Save Location'**
  String get saveLocation;

  /// No description provided for @exportPdf.
  ///
  /// In en, this message translates to:
  /// **'Export {count} Images to PDF'**
  String exportPdf(int count);

  /// No description provided for @connectMobileCamera.
  ///
  /// In en, this message translates to:
  /// **'Connect Mobile Camera'**
  String get connectMobileCamera;

  /// No description provided for @serverInterface.
  ///
  /// In en, this message translates to:
  /// **'Server Interface:'**
  String get serverInterface;

  /// No description provided for @scanWithApp.
  ///
  /// In en, this message translates to:
  /// **'Scan with Kaptchi mobile app'**
  String get scanWithApp;

  /// No description provided for @streamDisconnected.
  ///
  /// In en, this message translates to:
  /// **'Stream disconnected'**
  String get streamDisconnected;

  /// No description provided for @permissionsRequired.
  ///
  /// In en, this message translates to:
  /// **'Camera and Microphone permissions are required for streaming'**
  String get permissionsRequired;

  /// No description provided for @failedToStartStream.
  ///
  /// In en, this message translates to:
  /// **'Failed to start stream: {error}'**
  String failedToStartStream(Object error);

  /// No description provided for @failedToResumeStream.
  ///
  /// In en, this message translates to:
  /// **'Failed to resume stream: {error}'**
  String failedToResumeStream(Object error);

  /// No description provided for @rtmpNotSupported.
  ///
  /// In en, this message translates to:
  /// **'RTMP not supported on this platform'**
  String get rtmpNotSupported;

  /// No description provided for @errorStartingCamera.
  ///
  /// In en, this message translates to:
  /// **'Error starting camera: {error}'**
  String errorStartingCamera(Object error);

  /// No description provided for @captureFrameError.
  ///
  /// In en, this message translates to:
  /// **'Failed to capture frame'**
  String get captureFrameError;

  /// No description provided for @boundaryNotFoundError.
  ///
  /// In en, this message translates to:
  /// **'Failed to capture frame: boundary not found'**
  String get boundaryNotFoundError;

  /// No description provided for @errorCapturingFrame.
  ///
  /// In en, this message translates to:
  /// **'Error capturing frame: {error}'**
  String errorCapturingFrame(Object error);

  /// No description provided for @groupName.
  ///
  /// In en, this message translates to:
  /// **'Group Name'**
  String get groupName;

  /// No description provided for @selectFilters.
  ///
  /// In en, this message translates to:
  /// **'Select Filters:'**
  String get selectFilters;

  /// No description provided for @delete.
  ///
  /// In en, this message translates to:
  /// **'Delete'**
  String get delete;

  /// No description provided for @cancel.
  ///
  /// In en, this message translates to:
  /// **'Cancel'**
  String get cancel;

  /// No description provided for @save.
  ///
  /// In en, this message translates to:
  /// **'Save'**
  String get save;

  /// No description provided for @allFilters.
  ///
  /// In en, this message translates to:
  /// **'All Filters'**
  String get allFilters;

  /// No description provided for @filterGroups.
  ///
  /// In en, this message translates to:
  /// **'Filter Groups'**
  String get filterGroups;

  /// No description provided for @editFilterGroup.
  ///
  /// In en, this message translates to:
  /// **'Edit Filter Group'**
  String get editFilterGroup;

  /// No description provided for @addFilterGroup.
  ///
  /// In en, this message translates to:
  /// **'Add Filter Group'**
  String get addFilterGroup;

  /// No description provided for @activeFiltersCount.
  ///
  /// In en, this message translates to:
  /// **'{count} active filters'**
  String activeFiltersCount(int count);

  /// No description provided for @dragToReorder.
  ///
  /// In en, this message translates to:
  /// **'Drag to reorder. Top filters apply first.'**
  String get dragToReorder;

  /// No description provided for @capturedImages.
  ///
  /// In en, this message translates to:
  /// **'Captured Images'**
  String get capturedImages;

  /// No description provided for @noImagesCaptured.
  ///
  /// In en, this message translates to:
  /// **'No images captured'**
  String get noImagesCaptured;

  /// No description provided for @imageIndex.
  ///
  /// In en, this message translates to:
  /// **'Image {index}'**
  String imageIndex(int index);

  /// No description provided for @exportToPdf.
  ///
  /// In en, this message translates to:
  /// **'Export to PDF'**
  String get exportToPdf;

  /// No description provided for @exportPdfButton.
  ///
  /// In en, this message translates to:
  /// **'Export PDF'**
  String get exportPdfButton;

  /// No description provided for @close.
  ///
  /// In en, this message translates to:
  /// **'Close'**
  String get close;

  /// No description provided for @edit.
  ///
  /// In en, this message translates to:
  /// **'Edit'**
  String get edit;

  /// No description provided for @crop.
  ///
  /// In en, this message translates to:
  /// **'Crop'**
  String get crop;

  /// No description provided for @overlay.
  ///
  /// In en, this message translates to:
  /// **'Overlay'**
  String get overlay;

  /// No description provided for @fullscreenExit.
  ///
  /// In en, this message translates to:
  /// **'Exit Fullscreen'**
  String get fullscreenExit;

  /// No description provided for @transmissionTitle.
  ///
  /// In en, this message translates to:
  /// **'Transmitting'**
  String get transmissionTitle;

  /// No description provided for @stopTransmission.
  ///
  /// In en, this message translates to:
  /// **'Stop Transmission'**
  String get stopTransmission;

  /// No description provided for @cameraMode.
  ///
  /// In en, this message translates to:
  /// **'Camera Mode'**
  String get cameraMode;

  /// No description provided for @openFilters.
  ///
  /// In en, this message translates to:
  /// **'Open Filters'**
  String get openFilters;

  /// No description provided for @closeFilters.
  ///
  /// In en, this message translates to:
  /// **'Close Filters'**
  String get closeFilters;

  /// No description provided for @backToHome.
  ///
  /// In en, this message translates to:
  /// **'Back to Home'**
  String get backToHome;

  /// No description provided for @openGallery.
  ///
  /// In en, this message translates to:
  /// **'Open Gallery'**
  String get openGallery;

  /// No description provided for @closeGallery.
  ///
  /// In en, this message translates to:
  /// **'Close Gallery'**
  String get closeGallery;

  /// No description provided for @switchCamera.
  ///
  /// In en, this message translates to:
  /// **'Switch Camera'**
  String get switchCamera;

  /// No description provided for @lockZoom.
  ///
  /// In en, this message translates to:
  /// **'Lock Zoom to Pan'**
  String get lockZoom;

  /// No description provided for @unlockZoom.
  ///
  /// In en, this message translates to:
  /// **'Unlock Zoom'**
  String get unlockZoom;

  /// No description provided for @quickDraw.
  ///
  /// In en, this message translates to:
  /// **'Quick Draw'**
  String get quickDraw;

  /// No description provided for @captureFrame.
  ///
  /// In en, this message translates to:
  /// **'Capture Frame'**
  String get captureFrame;

  /// No description provided for @tryNextCamera.
  ///
  /// In en, this message translates to:
  /// **'Try Next Camera'**
  String get tryNextCamera;

  /// No description provided for @cameraFailed.
  ///
  /// In en, this message translates to:
  /// **'Camera failed to initialize'**
  String get cameraFailed;

  /// No description provided for @scanQrCode.
  ///
  /// In en, this message translates to:
  /// **'Scan QR Code'**
  String get scanQrCode;

  /// No description provided for @unsavedImagesTitle.
  ///
  /// In en, this message translates to:
  /// **'Unsaved Images'**
  String get unsavedImagesTitle;

  /// No description provided for @unsavedImagesMessage.
  ///
  /// In en, this message translates to:
  /// **'You have images in your gallery that are not exported. They will be lost if you exit. Are you sure?'**
  String get unsavedImagesMessage;

  /// No description provided for @exit.
  ///
  /// In en, this message translates to:
  /// **'Exit'**
  String get exit;

  /// No description provided for @pdfSaved.
  ///
  /// In en, this message translates to:
  /// **'Saved PDF to {path}'**
  String pdfSaved(String path);

  /// No description provided for @pdfExportError.
  ///
  /// In en, this message translates to:
  /// **'Error exporting PDF: {error}'**
  String pdfExportError(Object error);

  /// No description provided for @mediaServerRunning.
  ///
  /// In en, this message translates to:
  /// **'Media Server Running'**
  String get mediaServerRunning;

  /// No description provided for @mediaServerStopped.
  ///
  /// In en, this message translates to:
  /// **'Media Server Stopped'**
  String get mediaServerStopped;

  /// No description provided for @editImage.
  ///
  /// In en, this message translates to:
  /// **'Edit Image'**
  String get editImage;

  /// No description provided for @perspective.
  ///
  /// In en, this message translates to:
  /// **'Perspective'**
  String get perspective;

  /// No description provided for @rectangular.
  ///
  /// In en, this message translates to:
  /// **'Rectangular'**
  String get rectangular;

  /// No description provided for @cropFailed.
  ///
  /// In en, this message translates to:
  /// **'Crop Failed'**
  String get cropFailed;

  /// No description provided for @cropImageTitle.
  ///
  /// In en, this message translates to:
  /// **'Crop Image'**
  String get cropImageTitle;

  /// No description provided for @selectVideoSource.
  ///
  /// In en, this message translates to:
  /// **'Select Video Source'**
  String get selectVideoSource;

  /// No description provided for @localCameras.
  ///
  /// In en, this message translates to:
  /// **'Local Cameras'**
  String get localCameras;

  /// No description provided for @remoteStreams.
  ///
  /// In en, this message translates to:
  /// **'Remote Streams'**
  String get remoteStreams;

  /// No description provided for @mobileApp.
  ///
  /// In en, this message translates to:
  /// **'Mobile App'**
  String get mobileApp;

  /// No description provided for @connectViaQr.
  ///
  /// In en, this message translates to:
  /// **'Connect via QR Code'**
  String get connectViaQr;

  /// No description provided for @filterShakingStabilization.
  ///
  /// In en, this message translates to:
  /// **'Shaking Stabilization'**
  String get filterShakingStabilization;

  /// No description provided for @filterLightStabilization.
  ///
  /// In en, this message translates to:
  /// **'Light Stabilization'**
  String get filterLightStabilization;

  /// No description provided for @filterCornerSmoothing.
  ///
  /// In en, this message translates to:
  /// **'Corner Smoothing'**
  String get filterCornerSmoothing;

  /// No description provided for @filterSharpening.
  ///
  /// In en, this message translates to:
  /// **'Sharpening'**
  String get filterSharpening;

  /// No description provided for @filterContrastBoost.
  ///
  /// In en, this message translates to:
  /// **'Contrast Boost (CLAHE)'**
  String get filterContrastBoost;

  /// No description provided for @filterMovingAverage.
  ///
  /// In en, this message translates to:
  /// **'Moving Average'**
  String get filterMovingAverage;

  /// No description provided for @filterSmartObstacleRemoval.
  ///
  /// In en, this message translates to:
  /// **'Smart Obstacle Removal'**
  String get filterSmartObstacleRemoval;

  /// No description provided for @filterSmartWhiteboard.
  ///
  /// In en, this message translates to:
  /// **'Smart Whiteboard'**
  String get filterSmartWhiteboard;

  /// No description provided for @filterPersonRemoval.
  ///
  /// In en, this message translates to:
  /// **'Person Removal (AI)'**
  String get filterPersonRemoval;

  /// No description provided for @filterBlurLegacy.
  ///
  /// In en, this message translates to:
  /// **'Blur (Legacy)'**
  String get filterBlurLegacy;

  /// No description provided for @filterInvertColors.
  ///
  /// In en, this message translates to:
  /// **'Invert Colors'**
  String get filterInvertColors;

  /// No description provided for @filterWhiteboardLegacy.
  ///
  /// In en, this message translates to:
  /// **'Whiteboard (Legacy)'**
  String get filterWhiteboardLegacy;

  /// No description provided for @filterGroupStabilizers.
  ///
  /// In en, this message translates to:
  /// **'Stabilizers'**
  String get filterGroupStabilizers;

  /// No description provided for @mobileConnect.
  ///
  /// In en, this message translates to:
  /// **'Mobile Connect'**
  String get mobileConnect;

  /// No description provided for @filterGroupWhiteboard.
  ///
  /// In en, this message translates to:
  /// **'Whiteboard'**
  String get filterGroupWhiteboard;

  /// No description provided for @aboutKaptchi.
  ///
  /// In en, this message translates to:
  /// **'About Kaptchi'**
  String get aboutKaptchi;

  /// No description provided for @whatIsKaptchi.
  ///
  /// In en, this message translates to:
  /// **'What is Kaptchi?'**
  String get whatIsKaptchi;

  /// No description provided for @whatIsKaptchiDescription.
  ///
  /// In en, this message translates to:
  /// **'Kaptchi is specifically designed to help visually impaired students see the whiteboard better. It transforms your setup into a personal vision aid with high-contrast filters, stabilization, and magnification, ensuring you never miss a detail in class.'**
  String get whatIsKaptchiDescription;

  /// No description provided for @singleMonitorTip.
  ///
  /// In en, this message translates to:
  /// **'Single Monitor Tip'**
  String get singleMonitorTip;

  /// No description provided for @singleMonitorTipDescription.
  ///
  /// In en, this message translates to:
  /// **'If you only have one monitor and want to capture a specific window (like Zoom or Google Meet) without obstruction, we recommend installing the \"Virtual-Display-Driver\" from GitHub to create a virtual second screen.'**
  String get singleMonitorTipDescription;

  /// No description provided for @getVirtualDisplayDriver.
  ///
  /// In en, this message translates to:
  /// **'Get Virtual Display Driver'**
  String get getVirtualDisplayDriver;

  /// No description provided for @multiSourceCapable.
  ///
  /// In en, this message translates to:
  /// **'Multi-Source Capable'**
  String get multiSourceCapable;

  /// No description provided for @multiSourceDescription.
  ///
  /// In en, this message translates to:
  /// **'Switch instantly between multiple cameras and screen capture sources.'**
  String get multiSourceDescription;

  /// No description provided for @smartFilters.
  ///
  /// In en, this message translates to:
  /// **'Smart Filters'**
  String get smartFilters;

  /// No description provided for @smartFiltersDescription.
  ///
  /// In en, this message translates to:
  /// **'Enhance legibility with \"Whiteboard\", \"Stabilization\", and \"Sharpening\" filters.'**
  String get smartFiltersDescription;

  /// No description provided for @pdfExportDescription.
  ///
  /// In en, this message translates to:
  /// **'Capture snapshots to your gallery and export them directly as a PDF document.'**
  String get pdfExportDescription;

  /// No description provided for @mobileConnectDescription.
  ///
  /// In en, this message translates to:
  /// **'Use your phone as an external camera via the QR code connection.'**
  String get mobileConnectDescription;

  /// No description provided for @howToUse.
  ///
  /// In en, this message translates to:
  /// **'How to Use'**
  String get howToUse;

  /// No description provided for @step1.
  ///
  /// In en, this message translates to:
  /// **'Connect your cameras or ensure they are active.'**
  String get step1;

  /// No description provided for @step2.
  ///
  /// In en, this message translates to:
  /// **'Select a camera source from the list on the left.'**
  String get step2;

  /// No description provided for @step3.
  ///
  /// In en, this message translates to:
  /// **'Use the sidebar in the camera view to apply filters like \"Whiteboard\" for better contrast.'**
  String get step3;

  /// No description provided for @step4.
  ///
  /// In en, this message translates to:
  /// **'Take snapshots using the shutter button. They appear in the gallery.'**
  String get step4;

  /// No description provided for @step5.
  ///
  /// In en, this message translates to:
  /// **'Go to the home screen to review your gallery and export to PDF.'**
  String get step5;

  /// No description provided for @visitGithub.
  ///
  /// In en, this message translates to:
  /// **'Visit GitHub Repository'**
  String get visitGithub;

  /// No description provided for @madeWithLove.
  ///
  /// In en, this message translates to:
  /// **'Made with ❤️ by the Kaptchi Team'**
  String get madeWithLove;

  /// No description provided for @aboutTooltip.
  ///
  /// In en, this message translates to:
  /// **'About'**
  String get aboutTooltip;

  /// No description provided for @ipSelectionTooltip.
  ///
  /// In en, this message translates to:
  /// **'Select the IP address of your Wi-Fi or Local Network connection.\nUsually starts with 192.168.x.x or 10.x.x.x.\nEnsure your phone is on the same network.'**
  String get ipSelectionTooltip;

  /// No description provided for @screenCapture.
  ///
  /// In en, this message translates to:
  /// **'Screen Capture'**
  String get screenCapture;

  /// No description provided for @captureMonitor.
  ///
  /// In en, this message translates to:
  /// **'Capture {monitorName}'**
  String captureMonitor(Object monitorName);

  /// No description provided for @captureWindow.
  ///
  /// In en, this message translates to:
  /// **'Capture Window...'**
  String get captureWindow;

  /// No description provided for @selectWindowToCapture.
  ///
  /// In en, this message translates to:
  /// **'Select Window to Capture'**
  String get selectWindowToCapture;

  /// No description provided for @failedToStartScreenCapture.
  ///
  /// In en, this message translates to:
  /// **'Failed to start screen capture'**
  String get failedToStartScreenCapture;

  /// No description provided for @couldNotOpenLink.
  ///
  /// In en, this message translates to:
  /// **'Could not open link'**
  String get couldNotOpenLink;
}

class _AppLocalizationsDelegate
    extends LocalizationsDelegate<AppLocalizations> {
  const _AppLocalizationsDelegate();

  @override
  Future<AppLocalizations> load(Locale locale) {
    return SynchronousFuture<AppLocalizations>(lookupAppLocalizations(locale));
  }

  @override
  bool isSupported(Locale locale) =>
      <String>['en', 'he'].contains(locale.languageCode);

  @override
  bool shouldReload(_AppLocalizationsDelegate old) => false;
}

AppLocalizations lookupAppLocalizations(Locale locale) {
  // Lookup logic when only language code is specified.
  switch (locale.languageCode) {
    case 'en':
      return AppLocalizationsEn();
    case 'he':
      return AppLocalizationsHe();
  }

  throw FlutterError(
    'AppLocalizations.delegate failed to load unsupported locale "$locale". This is likely '
    'an issue with the localizations generation tool. Please file an issue '
    'on GitHub with a reproducible sample app and the gen-l10n configuration '
    'that was used.',
  );
}
