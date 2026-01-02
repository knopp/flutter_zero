import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate {

  var engine: FlutterEngine?

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    engine = FlutterEngine(name: "project", project: nil)
    GeneratedPluginRegistrant.register(with: engine!)
    engine?.run(withEntrypoint:nil)
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }
}
