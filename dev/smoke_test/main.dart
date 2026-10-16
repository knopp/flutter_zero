// Smoke test application for the headless flutter_zero engine.
//
// Printed lines are read from the flutter tool console by
// dev/smoke_test/run.py and asserted on. Do not change this file without
// updating dev/smoke_test/run.py.
//
// Semantics:
// - Top-level statics are preserved across a hot reload, but are re-initialized
//   when the isolate is replaced by a hot restart. `tickCount` therefore
//   distinguishes reload (continues incrementing) from restart (resets to 0).
// - `kVersion` lets the harness prove that *modified source* was actually
//   loaded, since a constant change only shows up in newly loaded code.
// - registerHotRestartListener is a flutter_zero dart:ui hook that fires just
//   before the isolate is destroyed (hot restart only, no reload).
import 'dart:async';
import 'dart:ui';

const String kVersion = 'A';

int tickCount = 0;

void main() {
  print('SMOKE:boot:$kVersion:$tickCount');
  Timer.periodic(const Duration(seconds: 1), (timer) {
    tickCount = tickCount + 1;
    print('SMOKE:tick:${versionName()}:$tickCount');
  });
  PlatformDispatcher.instance.registerHotRestartListener(() {
    print('SMOKE:pre-restart:$tickCount');
  });
}

String versionName() => kVersion;
