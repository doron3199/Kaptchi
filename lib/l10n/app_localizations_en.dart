// ignore: unused_import
import 'package:intl/intl.dart' as intl;
import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for English (`en`).
class AppLocalizationsEn extends AppLocalizations {
  AppLocalizationsEn([String locale = 'en']) : super(locale);

  @override
  String get appTitle => 'Kaptchi';

  @override
  String get settings => 'Settings';

  @override
  String get language => 'Language';

  @override
  String get english => 'English';

  @override
  String get hebrew => 'Hebrew';

  @override
  String get availableCameras => 'Available Cameras';

  @override
  String get noCamerasFound => 'No cameras found';

  @override
  String get exportSettings => 'Export Settings';

  @override
  String get fileName => 'File Name';

  @override
  String get saveLocation => 'Save Location';

  @override
  String exportPdf(int count) {
    return 'Export $count Images to PDF';
  }

  @override
  String get connectMobileCamera => 'Connect Mobile Camera';

  @override
  String get serverInterface => 'Server Interface:';

  @override
  String get scanWithApp => 'Scan with Kaptchi mobile app';

  @override
  String get streamDisconnected => 'Stream disconnected';

  @override
  String get permissionsRequired =>
      'Camera and Microphone permissions are required for streaming';

  @override
  String failedToStartStream(Object error) {
    return 'Failed to start stream: $error';
  }

  @override
  String failedToResumeStream(Object error) {
    return 'Failed to resume stream: $error';
  }

  @override
  String get rtmpNotSupported => 'RTMP not supported on this platform';

  @override
  String errorStartingCamera(Object error) {
    return 'Error starting camera: $error';
  }

  @override
  String get captureFrameError => 'Failed to capture frame';

  @override
  String get boundaryNotFoundError =>
      'Failed to capture frame: boundary not found';

  @override
  String errorCapturingFrame(Object error) {
    return 'Error capturing frame: $error';
  }

  @override
  String get groupName => 'Group Name';

  @override
  String get selectFilters => 'Select Filters:';

  @override
  String get delete => 'Delete';

  @override
  String get cancel => 'Cancel';

  @override
  String get save => 'Save';

  @override
  String get allFilters => 'All Filters';

  @override
  String get filterGroups => 'Filter Groups';

  @override
  String get editFilterGroup => 'Edit Filter Group';

  @override
  String get addFilterGroup => 'Add Filter Group';

  @override
  String activeFiltersCount(int count) {
    return '$count active filters';
  }

  @override
  String get dragToReorder => 'Drag to reorder. Top filters apply first.';

  @override
  String get capturedImages => 'Captured Images';

  @override
  String get noImagesCaptured => 'No images captured';

  @override
  String imageIndex(int index) {
    return 'Image $index';
  }

  @override
  String get exportToPdf => 'Export to PDF';

  @override
  String get exportPdfButton => 'Export PDF';

  @override
  String get close => 'Close';

  @override
  String get edit => 'Edit';

  @override
  String get crop => 'Crop';

  @override
  String get overlay => 'Overlay';

  @override
  String get fullscreenExit => 'Exit Fullscreen';

  @override
  String get transmissionTitle => 'Transmitting';

  @override
  String get stopTransmission => 'Stop Transmission';

  @override
  String get cameraMode => 'Camera Mode';

  @override
  String get openFilters => 'Open Filters';

  @override
  String get closeFilters => 'Close Filters';

  @override
  String get backToHome => 'Back to Home';

  @override
  String get openGallery => 'Open Gallery';

  @override
  String get closeGallery => 'Close Gallery';

  @override
  String get switchCamera => 'Switch Camera';

  @override
  String get lockZoom => 'Lock Zoom to Pan';

  @override
  String get unlockZoom => 'Unlock Zoom';

  @override
  String get quickDraw => 'Quick Draw';

  @override
  String get captureFrame => 'Capture Frame';

  @override
  String get tryNextCamera => 'Try Next Camera';

  @override
  String get cameraFailed => 'Camera failed to initialize';

  @override
  String get scanQrCode => 'Scan QR Code';

  @override
  String get unsavedImagesTitle => 'Unsaved Images';

  @override
  String get unsavedImagesMessage =>
      'You have images in your gallery that are not exported. They will be lost if you exit. Are you sure?';

  @override
  String get exit => 'Exit';

  @override
  String pdfSaved(String path) {
    return 'Saved PDF to $path';
  }

  @override
  String pdfExportError(Object error) {
    return 'Error exporting PDF: $error';
  }

  @override
  String get mediaServerRunning => 'Media Server Running';

  @override
  String get mediaServerStopped => 'Media Server Stopped';

  @override
  String get editImage => 'Edit Image';

  @override
  String get perspective => 'Perspective';

  @override
  String get rectangular => 'Rectangular';

  @override
  String get cropFailed => 'Crop Failed';

  @override
  String get cropImageTitle => 'Crop Image';

  @override
  String get selectVideoSource => 'Select Video Source';

  @override
  String get localCameras => 'Local Cameras';

  @override
  String get remoteStreams => 'Remote Streams';

  @override
  String get mobileApp => 'Mobile App';

  @override
  String get connectViaQr => 'Connect via QR Code';

  @override
  String get filterShakingStabilization => 'Shaking Stabilization';

  @override
  String get filterLightStabilization => 'Light Stabilization';

  @override
  String get filterCornerSmoothing => 'Corner Smoothing';

  @override
  String get filterSharpening => 'Sharpening';

  @override
  String get filterContrastBoost => 'Contrast Boost (CLAHE)';

  @override
  String get filterMovingAverage => 'Moving Average';

  @override
  String get filterSmartObstacleRemoval => 'Smart Obstacle Removal';

  @override
  String get filterSmartWhiteboard => 'Smart Whiteboard';

  @override
  String get filterPersonRemoval => 'Person Removal (AI)';

  @override
  String get filterBlurLegacy => 'Blur (Legacy)';

  @override
  String get filterInvertColors => 'Invert Colors';

  @override
  String get filterWhiteboardLegacy => 'Whiteboard (Legacy)';

  @override
  String get filterThresholdSensitivity => 'Threshold:';

  @override
  String get filterGroupStabilizers => 'Stabilizers';

  @override
  String get mobileConnect => 'Mobile Connect';

  @override
  String get filterGroupWhiteboard => 'Whiteboard';

  @override
  String get aboutKaptchi => 'About Kaptchi';

  @override
  String get whatIsKaptchi => 'What is Kaptchi?';

  @override
  String get whatIsKaptchiDescription =>
      'Kaptchi is specifically designed to help visually impaired students see the whiteboard better. It transforms your setup into a personal vision aid with high-contrast filters, stabilization, and magnification, ensuring you never miss a detail in class.';

  @override
  String get singleMonitorTip => 'Single Monitor Tip';

  @override
  String get singleMonitorTipDescription =>
      'If you only have one monitor and want to capture a specific window (like Zoom or Google Meet) without obstruction, we recommend installing the \"Virtual-Display-Driver\" from GitHub to create a virtual second screen.';

  @override
  String get getVirtualDisplayDriver => 'Get Virtual Display Driver';

  @override
  String get multiSourceCapable => 'Multi-Source Capable';

  @override
  String get multiSourceDescription =>
      'Switch instantly between multiple cameras and screen capture sources.';

  @override
  String get smartFilters => 'Smart Filters';

  @override
  String get smartFiltersDescription =>
      'Enhance legibility with \"Whiteboard\", \"Stabilization\", and \"Sharpening\" filters.';

  @override
  String get pdfExportDescription =>
      'Capture snapshots to your gallery and export them directly as a PDF document.';

  @override
  String get mobileConnectDescription =>
      'Use your phone as an external camera via the QR code connection.';

  @override
  String get howToUse => 'How to Use';

  @override
  String get step1 => 'Connect your cameras or ensure they are active.';

  @override
  String get step2 => 'Select a camera source from the list on the left.';

  @override
  String get step3 =>
      'Use the sidebar in the camera view to apply filters like \"Whiteboard\" for better contrast.';

  @override
  String get step4 =>
      'Take snapshots using the shutter button. They appear in the gallery.';

  @override
  String get step5 =>
      'Go to the home screen to review your gallery and export to PDF.';

  @override
  String get visitGithub => 'Visit GitHub Repository';

  @override
  String get madeWithLove => 'Made with ❤️ by the Kaptchi Team';

  @override
  String get aboutTooltip => 'About';

  @override
  String get ipSelectionTooltip =>
      'Select the IP address of your Wi-Fi or Local Network connection.\nUsually starts with 192.168.x.x or 10.x.x.x.\nEnsure your phone is on the same network.';

  @override
  String get screenCapture => 'Screen Capture';

  @override
  String captureMonitor(Object monitorName) {
    return 'Capture $monitorName';
  }

  @override
  String get captureWindow => 'Capture Window...';

  @override
  String get selectWindowToCapture => 'Select Window to Capture';

  @override
  String get failedToStartScreenCapture => 'Failed to start screen capture';

  @override
  String get couldNotOpenLink => 'Could not open link';
}
