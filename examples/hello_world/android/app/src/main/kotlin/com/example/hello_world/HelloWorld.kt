package com.example.hello_world

import android.app.Application
import io.flutter.FlutterInjector
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterEngineCache
import io.flutter.embedding.engine.dart.DartExecutor

class HelloWorld : Application() {

    companion object {
        // Use a constant key so Activities/Services can fetch the engine later.
        const val ENGINE_ID = "headless_engine"
    }

    private var engine: FlutterEngine? = null

    override fun onCreate() {
        super.onCreate()

        // Ensure Flutter is initialized (loads engine resources, etc.)
        // Usually automatic, but doing it explicitly makes startup behavior predictable.
        FlutterInjector.instance().flutterLoader().startInitialization(this)
        FlutterInjector.instance().flutterLoader().ensureInitializationComplete(this, null)

        // Create a FlutterEngine not tied to any Activity.
        val flutterEngine = FlutterEngine(this)

        // Start executing Dart code. This runs the "main()" of your Flutter app by default.
        val entrypoint = DartExecutor.DartEntrypoint.createDefault()
        flutterEngine.dartExecutor.executeDartEntrypoint(entrypoint)

        // Optionally: register MethodChannels / EventChannels here, once the engine exists.
        // Example: setupChannels(flutterEngine)

        // Cache it so other components can reuse it without recreating.
        FlutterEngineCache.getInstance().put(ENGINE_ID, flutterEngine)

        engine = flutterEngine
    }

    override fun onTerminate() {
        // onTerminate is not reliable in production, but safe to keep for emulators/tests.
        engine?.destroy()
        engine = null
        super.onTerminate()
    }
}
