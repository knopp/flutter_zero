# Flutter Zero

Flutter Zero is a stripped down version of [Flutter](https://flutter.dev) with most of the `dart:ui` removed. There is no built-in UI support, Skia or Impeller. It is built to be compatible with existing Flutter tooling, so the `flutter` tool can be used to create, build, and run Flutter Zero applications and the IDE plugins will keep working as expected too (minus the Flutter UI related parts obviously).

In other words Flutter Zero is a minimal Dart runtime that can be used to build applications on all platforms supported by Flutter, but without making any assumptions about the UI layer.

This is done in an effort to explore new use-cases for Dart, such as writing applications with native UI toolkits through [Dart interop](https://dart.dev/interop) or building `dart:ui` as a separate package not built into the Flutter engine.


# Hypothetically Asked Questions

### Can this be used right now?

Not unless you want to build the engine yourself. The CI part that builds and deploys Flutter Zero engine binaries is still missing.

### Why?

Why not?

### No seriously, why though?

Because I'm not too happy how tightly `dart:ui` is coupled with Flutter, and to be fair, I'm not too happy with how `dart:ui` is built either. I'm fairly certain that every decision made when building `dart:ui` made perfect sense at the time, but the cumulative effect is that `dart:ui` is a monolithic blob with layers of abstractions and indirections that are not always at the right place or even necessary.

It was built with assumptions and constraints that are not valid anymore. The threading model has changed. The FFI story has improved significantly. Bidirectional synchronous interaction between Dart and platform APIs is now possible. The untyped, underspecified, asynchronous and often adhoc platform channels are not needed anymore.  Dart has now support for [native assets](https://github.com/dart-lang/native) meaning that packages can easily contribute native code with custom built-steps.

All of this combined means that it now *might* be feasible to build a modular version of `dart:ui`, decoupled from the engine, with proper Dart interfaces and platform specific FFI/JNI implementations.

### Why is this a new repository and not a technically a fork?

This contains a very tiny subset of the Flutter codebase. The original Flutter repository is huge and vast majority of the history is completely irrelevant. This prioritizes fast checkout and smaller disk usage.

### What is the threading model?

All Dart code runs on the platform thread. No other threading configurations are supported.

### Why does this still have Flutter in the name given that most of what makes Flutter, Flutter is gone?

TBH, this is a very experimental project and I don't feel like obsessing over the name is a high priority right now. The compatibility with tooling is a key feature (I don't need to spend a year trying to rewrite the `flutter_tool` from scratch), and eventually, with a lot of luck and motivation, it should be possible to write a `dart:ui` reimplementation and an abstraction layer good enough so that regular Flutter applications could run on top of Flutter Zero with minimal changes.
