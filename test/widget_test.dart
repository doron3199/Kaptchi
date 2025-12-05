import 'package:flutter_test/flutter_test.dart';
import 'package:kaptchi_flutter/main.dart';

void main() {
  testWidgets('App smoke test', (WidgetTester tester) async {
    // Build our app and trigger a frame.
    await tester.pumpWidget(const MyApp());

    // Verify that the app starts and shows the home screen title.
    // Note: Depending on the initial state, it might show a loading screen or the home screen.
    // Assuming it goes to HomeScreen which has 'Kaptchi Start' in the AppBar on desktop.
    
    expect(find.text('Kaptchi Start'), findsOneWidget);
  });
}
