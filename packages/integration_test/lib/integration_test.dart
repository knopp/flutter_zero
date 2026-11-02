/// The bare minimum `flutter test integration_test` requires; the generated
/// test bootstrap calls the members below.
library;

/// Called by the test bootstrap; does nothing.
class IntegrationTestWidgetsFlutterBinding {
  IntegrationTestWidgetsFlutterBinding._();

  static final _instance = IntegrationTestWidgetsFlutterBinding._();

  /// Returns the binding.
  static IntegrationTestWidgetsFlutterBinding ensureInitialized() => _instance;
}

/// Called by the test bootstrap; does nothing.
abstract final class VmServiceProxyGoldenFileComparator {
  /// Does nothing.
  static void useIfRunningOnDevice() {}
}
