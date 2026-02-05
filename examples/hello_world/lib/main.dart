import 'dart:ui';

void main() {
  print('Hello there...');

  PlatformDispatcher.instance.registerHotRestartListener(() {
    print('Hot restart listener called!');
  });
}
