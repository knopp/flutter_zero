# integration_test

Runs integration tests on a device with
`flutter test integration_test -d <device>`. Contains the bare minimum the
tool requires; tests are written with `package:test`.

```yaml
dev_dependencies:
  integration_test:
    sdk: flutter
  test: any
```

```dart
// integration_test/app_test.dart
import 'package:test/test.dart';

void main() {
  test('runs in the app', () {
    expect(1 + 1, 2);
  });
}
```
