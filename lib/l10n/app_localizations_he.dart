// ignore: unused_import
import 'package:intl/intl.dart' as intl;
import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for Hebrew (`he`).
class AppLocalizationsHe extends AppLocalizations {
  AppLocalizationsHe([String locale = 'he']) : super(locale);

  @override
  String get appTitle => 'Kaptchi';

  @override
  String get settings => 'הגדרות';

  @override
  String get language => 'שפה';

  @override
  String get english => 'אנגלית';

  @override
  String get hebrew => 'עברית';

  @override
  String get availableCameras => 'מצלמות זמינות';

  @override
  String get noCamerasFound => 'לא נמצאו מצלמות';

  @override
  String get exportSettings => 'הגדרות ייצוא';

  @override
  String get fileName => 'שם הקובץ';

  @override
  String get saveLocation => 'מיקום השמירה';

  @override
  String exportPdf(int count) {
    return 'ייצא $count תמונות ל-PDF';
  }

  @override
  String get connectMobileCamera => 'חבר מצלמת טלפון';

  @override
  String get serverInterface => 'ממשק שרת:';

  @override
  String get scanWithApp => 'סרוק עם אפליקציית Kaptchi';

  @override
  String get streamDisconnected => 'השידור התנתק';

  @override
  String get permissionsRequired => 'נדרשות הרשאות מצלמה ומיקרופון לשידור';

  @override
  String failedToStartStream(Object error) {
    return 'הפעלת השידור נכשלה: $error';
  }

  @override
  String failedToResumeStream(Object error) {
    return 'חידוש השידור נכשל: $error';
  }

  @override
  String get rtmpNotSupported => 'RTMP אינו נתמך בפלטפורמה זו';

  @override
  String errorStartingCamera(Object error) {
    return 'שגיאה בהפעלת המצלמה: $error';
  }

  @override
  String get captureFrameError => 'נכשל בלכידת הפריים';

  @override
  String get boundaryNotFoundError => 'נכשל בלכידת הפריים: גבול לא נמצא';

  @override
  String errorCapturingFrame(Object error) {
    return 'שגיאה בלכידת הפריים: $error';
  }

  @override
  String get groupName => 'שם קבוצה';

  @override
  String get selectFilters => 'בחר מסננים:';

  @override
  String get delete => 'מחק';

  @override
  String get cancel => 'ביטול';

  @override
  String get save => 'שמור';

  @override
  String get allFilters => 'כל המסננים';

  @override
  String get filterGroups => 'קבוצות מסננים';

  @override
  String get editFilterGroup => 'ערוך קבוצת מסננים';

  @override
  String get addFilterGroup => 'הוסף קבוצת מסננים';

  @override
  String activeFiltersCount(int count) {
    return '$count מסננים פעילים';
  }

  @override
  String get dragToReorder => 'גרור כדי לסדר מחדש. המסננים העליונים חלים קודם.';

  @override
  String get capturedImages => 'תמונות שנלכדו';

  @override
  String get noImagesCaptured => 'אין תמונות';

  @override
  String imageIndex(int index) {
    return 'תמונה $index';
  }

  @override
  String get exportToPdf => 'ייצא ל-PDF';

  @override
  String get exportPdfButton => 'ייצא PDF';

  @override
  String get close => 'סגור';

  @override
  String get edit => 'ערוך';

  @override
  String get crop => 'חתוך';

  @override
  String get overlay => 'שכבה';

  @override
  String get fullscreenExit => 'צא ממסך מלא';

  @override
  String get transmissionTitle => 'משדר';

  @override
  String get stopTransmission => 'עצור שידור';

  @override
  String get cameraMode => 'מצב מצלמה';

  @override
  String get openFilters => 'פתח מסננים';

  @override
  String get closeFilters => 'סגור מסננים';

  @override
  String get backToHome => 'חזרה לבית';

  @override
  String get openGallery => 'פתח גלריה';

  @override
  String get closeGallery => 'סגור גלריה';

  @override
  String get switchCamera => 'החלף מצלמה';

  @override
  String get lockZoom => 'נעל זום לגלילה';

  @override
  String get unlockZoom => 'שחרר זום';

  @override
  String get quickDraw => 'ציור מהיר';

  @override
  String get captureFrame => 'צלם תמונה';

  @override
  String get tryNextCamera => 'נסה מצלמה הבאה';

  @override
  String get cameraFailed => 'המצלמה נכשלה באתחול';

  @override
  String get scanQrCode => 'סרוק קוד QR';

  @override
  String get unsavedImagesTitle => 'תמונות לא שמורות';

  @override
  String get unsavedImagesMessage =>
      'יש לך תמונות בגלריה שלא יוצאו. הן יאבדו אם תצא. האם אתה בטוח?';

  @override
  String get exit => 'יציאה';

  @override
  String pdfSaved(String path) {
    return 'PDF נשמר בנתיב $path';
  }

  @override
  String pdfExportError(Object error) {
    return 'שגיאה בייצוא PDF: $error';
  }

  @override
  String get mediaServerRunning => 'שרת מדיה פועל';

  @override
  String get mediaServerStopped => 'שרת מדיה כבוי';

  @override
  String get editImage => 'ערוך תמונה';

  @override
  String get perspective => 'פרספקטיבה';

  @override
  String get rectangular => 'מלבני';

  @override
  String get cropFailed => 'חיתוך נכשל';

  @override
  String get cropImageTitle => 'חתוך תמונה';

  @override
  String get selectVideoSource => 'בחר מקור וידאו';

  @override
  String get localCameras => 'מצלמות מקומיות';

  @override
  String get remoteStreams => 'שידורים מרוחקים';

  @override
  String get mobileApp => 'אפליקציה ניידת';

  @override
  String get connectViaQr => 'התחבר באמצעות קוד QR';

  @override
  String get filterShakingStabilization => 'ייצוב רעידות';

  @override
  String get filterLightStabilization => 'ייצוב תאורה';

  @override
  String get filterCornerSmoothing => 'החלקת פינות';

  @override
  String get filterSharpening => 'חידוד';

  @override
  String get filterContrastBoost => 'הגברת ניגודיות (CLAHE)';

  @override
  String get filterMovingAverage => 'ממוצע נע';

  @override
  String get filterSmartObstacleRemoval => 'הסרת מכשולים חכמה';

  @override
  String get filterSmartWhiteboard => 'לוח לבן חכם';

  @override
  String get filterPersonRemoval => 'הסרת אנשים (AI)';

  @override
  String get filterBlurLegacy => 'טשטוש (ישן)';

  @override
  String get filterInvertColors => 'היפוך צבעים';

  @override
  String get filterWhiteboardLegacy => 'לוח לבן (ישן)';

  @override
  String get filterThresholdSensitivity => 'רגישות:';

  @override
  String get filterGroupStabilizers => 'מייצבים';

  @override
  String get mobileConnect => 'חבר מצלמת טלפון';

  @override
  String get filterGroupWhiteboard => 'לוח לבן';

  @override
  String get aboutKaptchi => 'אודות Kaptchi';

  @override
  String get whatIsKaptchi => 'מה זה Kaptchi?';

  @override
  String get whatIsKaptchiDescription =>
      'Kaptchi נוצרה במיוחד כדי לעזור לסטודנטים לקויי ראייה לראות את הלוח טוב יותר. היא הופכת את המחשב שלך לעזר ראייה אישי עם מסננים חכמים לשיפור ניגודיות, ייצוב והגדלה, כדי שלא תפספס אף פרט בשיעור.';

  @override
  String get singleMonitorTip => 'טיפ למסך יחיד';

  @override
  String get singleMonitorTipDescription =>
      'אם יש לך רק מסך אחד ואתה רוצה לצלם חלון מסוים (כמו זום או גוגל מיט) בלי שהחלון יסתיר לך, אנחנו ממליצים להתקין את \"Virtual-Display-Driver\" מ-GitHub כדי ליצור מסך שני וירטואלי.';

  @override
  String get getVirtualDisplayDriver => 'הורד דרייבר למסך וירטואלי';

  @override
  String get multiSourceCapable => 'תמיכה במקורות מרובים';

  @override
  String get multiSourceDescription =>
      'החלף באופן מיידי בין מצלמות מרובות ולכידת מסך.';

  @override
  String get smartFilters => 'מסננים חכמים';

  @override
  String get smartFiltersDescription =>
      'שפר את קריאות הטקסט עם מסנני \"לוח לבן\", \"ייצוב\" ו\"חידוד\".';

  @override
  String get pdfExportDescription =>
      'צלם תמונות לגלריה וייצא אותן ישירות לקובץ PDF.';

  @override
  String get mobileConnectDescription =>
      'השתמש בטלפון כמצלמה חיצונית באמצעות סריקת קוד QR.';

  @override
  String get howToUse => 'איך להשתמש';

  @override
  String get step1 => 'חבר את המצלמות או וודא שהן פעילות.';

  @override
  String get step2 => 'בחר מקור מצלמה מהרשימה בצד שמאל.';

  @override
  String get step3 =>
      'השתמש בסרגל הצד במסך המצלמה כדי להפעיל מסננים כמו \"לוח לבן\" לניגודיות טובה יותר.';

  @override
  String get step4 => 'צלם תמונות באמצעות כפתור הצילום. הן יופיעו בגלריה.';

  @override
  String get step5 => 'חזור למסך הבית כדי לעבור על הגלריה ולייצא ל-PDF.';

  @override
  String get visitGithub => 'בקר בגיטהאב';

  @override
  String get madeWithLove => 'נבנה באהבה על ידי צוות Kaptchi';

  @override
  String get aboutTooltip => 'אודות';

  @override
  String get ipSelectionTooltip =>
      'בחר את כתובת ה-IP של חיבור ה-Wi-Fi או הרשת המקומית שלך.\nבדרך כלל מתחיל ב-192.168.x.x או 10.x.x.x.\nוודא שהטלפון מחובר לאותה רשת.';

  @override
  String get screenCapture => 'לכידת מסך';

  @override
  String captureMonitor(Object monitorName) {
    return 'לכוד $monitorName';
  }

  @override
  String get captureWindow => 'לכוד חלון...';

  @override
  String get selectWindowToCapture => 'בחר חלון ללכידה';

  @override
  String get failedToStartScreenCapture => 'נכשל בהפעלת לכידת מסך';

  @override
  String get couldNotOpenLink => 'לא ניתן לפתוח קישור';
}
