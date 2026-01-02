// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Looper;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.UiThread;
import androidx.annotation.VisibleForTesting;
import com.getkeepsafe.relinker.ReLinker;
import io.flutter.Log;
import io.flutter.embedding.engine.FlutterEngine.EngineLifecycleListener;
import io.flutter.embedding.engine.dart.PlatformMessageHandler;
import io.flutter.embedding.engine.deferredcomponents.DeferredComponentManager;
import io.flutter.plugin.localization.LocalizationPlugin;
import io.flutter.util.Preconditions;
import io.flutter.view.FlutterCallbackInformation;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArraySet;
import java.util.concurrent.locks.ReentrantReadWriteLock;

/**
 * Interface between Flutter embedding's Java code and Flutter engine's C/C++
 * code.
 *
 * <p>
 * Flutter's engine is built with C/C++. The Android Flutter embedding is
 * responsible for
 * coordinating Android OS events and app user interactions with the C/C++
 * engine. Such coordination
 * requires messaging from an Android app in Java code to the C/C++ engine code.
 * This communication
 * requires a JNI (Java Native Interface) API to cross the Java/native boundary.
 *
 * <p>
 * The entirety of Flutter's JNI API is codified in {@code FlutterJNI}. There
 * are multiple
 * reasons that all such calls are centralized in one class. First, JNI calls
 * are inherently static
 * and contain no Java implementation, therefore there is little reason to
 * associate calls with
 * different classes. Second, every JNI call must be registered in C/C++ code
 * and this registration
 * becomes more complicated with every additional Java class that contains JNI
 * calls. Third, most
 * Android developers are not familiar with native development or JNI
 * intricacies, therefore it is
 * in the interest of future maintenance to reduce the API surface that includes
 * JNI declarations.
 * Thus, all Flutter JNI calls are centralized in {@code FlutterJNI}.
 *
 * <p>
 * Despite the fact that individual JNI calls are inherently static, there is
 * state that exists
 * within {@code FlutterJNI}. Most calls within {@code FlutterJNI} correspond to
 * a specific
 * "platform view", of which there may be many. Therefore, each
 * {@code FlutterJNI} instance holds
 * onto a "native platform view ID" after {@link #attachToNative()}, which is
 * shared with the native
 * C/C++ engine code. That ID is passed to every platform-view-specific native
 * method. ID management
 * is handled within {@code FlutterJNI} so that developers don't have to hold
 * onto that ID.
 *
 * <p>
 * To connect part of an Android app to Flutter's C/C++ engine, instantiate a
 * {@code FlutterJNI}
 * and then attach it to the native side:
 *
 * <pre>{@code
 * // Instantiate FlutterJNI and attach to the native side.
 * FlutterJNI flutterJNI = new FlutterJNI();
 * flutterJNI.attachToNative();
 *
 * // Use FlutterJNI as desired. flutterJNI.dispatchPointerDataPacket(...);
 *
 * // Destroy the connection to the native side and cleanup.
 * flutterJNI.detachFromNativeAndReleaseResources();
 * }</pre>
 *
 * <p>
 * To receive callbacks for certain events that occur on the native side,
 * register listeners:
 *
 * <ol>
 * <li>{@link #addEngineLifecycleListener(FlutterEngine.EngineLifecycleListener)}
 * </ol>
 *
 * To facilitate platform messages between Java and Dart running in Flutter,
 * register a handler:
 *
 * <p>
 * {@link #setPlatformMessageHandler(PlatformMessageHandler)}
 *
 * <p>
 * To invoke a native method that is not associated with a platform view, invoke
 * it statically:
 *
 * <p>
 * {@code bool enabled = FlutterJNI.getIsSoftwareRenderingEnabled(); }
 */
@Keep
public class FlutterJNI {
  private static final String TAG = "FlutterJNI";
  // This serializes the invocation of platform message responses and the
  // attachment and detachment of the shell holder. This ensures that we don't
  // detach FlutterJNI on the platform thread while a background thread invokes
  // a message response. Typically accessing the shell holder happens on the
  // platform thread and doesn't require locking.
  private ReentrantReadWriteLock shellHolderLock = new ReentrantReadWriteLock();

  // Prefer using the FlutterJNI.Factory so it's easier to test.
  public FlutterJNI() {
    // We cache the main looper so that we can ensure calls are made on the main
    // thread
    // without consistently paying the synchronization cost of getMainLooper().
    mainLooper = Looper.getMainLooper();
  }

  /**
   * A factory for creating {@code FlutterJNI} instances. Useful for FlutterJNI
   * injections during
   * tests.
   */
  public static class Factory {
    /** @return a {@link FlutterJNI} instance. */
    public FlutterJNI provideFlutterJNI() {
      return new FlutterJNI();
    }
  }

  // BEGIN Methods related to loading for FlutterLoader.
  /**
   * Loads the libflutter.so C++ library.
   *
   * <p>
   * This must be called before any other native methods, and can be overridden by
   * tests to avoid
   * loading native libraries.
   *
   * <p>
   * This method should only be called once across all FlutterJNI instances.
   */
  public void loadLibrary(Context context) {
    if (FlutterJNI.loadLibraryCalled) {
      Log.w(TAG, "FlutterJNI.loadLibrary called more than once");
    }
    ReLinker.log(msg -> Log.d(TAG, msg)).loadLibrary(context, "flutter");
    FlutterJNI.loadLibraryCalled = true;
  }

  private static boolean loadLibraryCalled = false;

  private static native void nativeInit(
      @NonNull Context context,
      @NonNull String[] args,
      @Nullable String bundlePath,
      @NonNull String appStoragePath,
      @NonNull String engineCachesPath,
      long initTimeMillis,
      int apiLevel);

  /**
   * Perform one time initialization of the Dart VM and Flutter engine.
   *
   * <p>
   * This method must be called only once. Calling more than once will cause an
   * exception.
   *
   * @param context          The application context.
   * @param args             Arguments to the Dart VM/Flutter engine.
   * @param bundlePath       For JIT runtimes, the path to the Dart kernel file
   *                         for the application.
   * @param appStoragePath   The path to the application data directory.
   * @param engineCachesPath The path to the application cache directory.
   * @param initTimeMillis   The time, in milliseconds, taken for initialization.
   * @param apiLevel         The current Android API level.
   */
  public void init(
      @NonNull Context context,
      @NonNull String[] args,
      @Nullable String bundlePath,
      @NonNull String appStoragePath,
      @NonNull String engineCachesPath,
      long initTimeMillis,
      int apiLevel) {
    if (FlutterJNI.initCalled) {
      Log.w(TAG, "FlutterJNI.init called more than once");
    }

    FlutterJNI.nativeInit(
        context, args, bundlePath, appStoragePath, engineCachesPath, initTimeMillis, apiLevel);
    FlutterJNI.initCalled = true;
  }

  private static boolean initCalled = false;
  // END methods related to FlutterLoader

  // This is set from native code via JNI.
  @Nullable
  private static String vmServiceUri;

  private native boolean nativeGetIsSoftwareRenderingEnabled();

  /**
   * Checks launch settings for whether software rendering is requested.
   *
   * <p>
   * The value is the same per program.
   */
  @UiThread
  public boolean getIsSoftwareRenderingEnabled() {
    return nativeGetIsSoftwareRenderingEnabled();
  }

  /**
   * VM Service URI for the VM instance.
   *
   * <p>
   * Its value is set by the native engine once
   * {@link #init(Context, String[], String, String,
   * String, long, int)} is run.
   */
  @Nullable
  public static String getVMServiceUri() {
    return vmServiceUri;
  }

  @NonNull
  @Deprecated
  public static native FlutterCallbackInformation nativeLookupCallbackInformation(long handle);

  // ----- End Engine FlutterTextUtils Methods ----

  // Below represents the stateful part of the FlutterJNI instances that aren't
  // static per program.
  // Conceptually, it represents a native shell instance.

  @Nullable
  private Long nativeShellHolderId;
  @Nullable
  private PlatformMessageHandler platformMessageHandler;
  @Nullable
  private LocalizationPlugin localizationPlugin;

  @Nullable
  private DeferredComponentManager deferredComponentManager;

  @NonNull
  private final Set<EngineLifecycleListener> engineLifecycleListeners = new CopyOnWriteArraySet<>();

  @NonNull
  private final Looper mainLooper; // cached to avoid synchronization on repeat access.

  // ------ Start Native Attach/Detach Support ----
  /**
   * Returns true if this instance of {@code FlutterJNI} is connected to Flutter's
   * native engine via
   * a Java Native Interface (JNI).
   */
  public boolean isAttached() {
    return nativeShellHolderId != null;
  }

  /**
   * Attaches this {@code FlutterJNI} instance to Flutter's native engine, which
   * allows for
   * communication between Android code and Flutter's platform agnostic engine.
   *
   * <p>
   * This method must not be invoked if {@code FlutterJNI} is already attached to
   * native.
   */
  @UiThread
  public void attachToNative() {
    ensureRunningOnMainThread();
    ensureNotAttachedToNative();
    shellHolderLock.writeLock().lock();
    try {
      nativeShellHolderId = performNativeAttach(this);
    } finally {
      shellHolderLock.writeLock().unlock();
    }
  }

  @VisibleForTesting
  public long performNativeAttach(@NonNull FlutterJNI flutterJNI) {
    return nativeAttach(flutterJNI);
  }

  private native long nativeAttach(@NonNull FlutterJNI flutterJNI);

  /**
   * Spawns a new FlutterJNI instance from the current instance.
   *
   * <p>
   * This creates another native shell from the current shell. This causes the 2
   * shells to re-use
   * some of the shared resources, reducing the total memory consumption versus
   * creating a new
   * FlutterJNI by calling its standard constructor.
   *
   * <p>
   * This can only be called once the current FlutterJNI instance is attached by
   * calling {@link
   * #attachToNative()}.
   *
   * <p>
   * Static methods that should be only called once such as
   * {@link #init(Context, String[],
   * String, String, String, long, int)} shouldn't be called again on the spawned
   * FlutterJNI
   * instance.
   */
  @UiThread
  @NonNull
  public FlutterJNI spawn(
      @Nullable String entrypointFunctionName,
      @Nullable String pathToEntrypointFunction,
      @Nullable List<String> entrypointArgs,
      long engineId) {
    ensureRunningOnMainThread();
    ensureAttachedToNative();
    FlutterJNI spawnedJNI = nativeSpawn(
        nativeShellHolderId,
        entrypointFunctionName,
        pathToEntrypointFunction,
        entrypointArgs,
        engineId);
    Preconditions.checkState(
        spawnedJNI.nativeShellHolderId != null && spawnedJNI.nativeShellHolderId != 0,
        "Failed to spawn new JNI connected shell from existing shell.");

    return spawnedJNI;
  }

  private native FlutterJNI nativeSpawn(
      long nativeSpawningShellId,
      @Nullable String entrypointFunctionName,
      @Nullable String pathToEntrypointFunction,
      @Nullable List<String> entrypointArgs,
      long engineId);

  /**
   * Detaches this {@code FlutterJNI} instance from Flutter's native engine, which
   * precludes any
   * further communication between Android code and Flutter's platform agnostic
   * engine.
   *
   * <p>
   * This method must not be invoked if {@code FlutterJNI} is not already attached
   * to native.
   *
   * <p>
   * Invoking this method will result in the release of all native-side resources
   * that were set
   * up during {@link #attachToNative()} or
   * {@link #spawn(String, String, List, long)}, or
   * accumulated thereafter.
   *
   * <p>
   * It is permissible to re-attach this instance to native after detaching it
   * from native.
   */
  @UiThread
  public void detachFromNativeAndReleaseResources() {
    ensureRunningOnMainThread();
    ensureAttachedToNative();
    shellHolderLock.writeLock().lock();
    try {
      nativeDestroy(nativeShellHolderId);
      nativeShellHolderId = null;
    } finally {
      shellHolderLock.writeLock().unlock();
    }
  }

  private native void nativeDestroy(long nativeShellHolderId);

  private void ensureNotAttachedToNative() {
    if (nativeShellHolderId != null) {
      throw new RuntimeException(
          "Cannot execute operation because FlutterJNI is attached to native.");
    }
  }

  private void ensureAttachedToNative() {
    if (nativeShellHolderId == null) {
      throw new RuntimeException(
          "Cannot execute operation because FlutterJNI is not attached to native.");
    }
  }
  // ------ End Native Attach/Detach Support ----

  // ------ Start Dart Execution Support -------
  /**
   * Executes a Dart entrypoint.
   *
   * <p>
   * This can only be done once per JNI attachment because a Dart isolate can only
   * be entered
   * once.
   */
  @UiThread
  public void runBundleAndSnapshotFromLibrary(
      @NonNull String bundlePath,
      @Nullable String entrypointFunctionName,
      @Nullable String pathToEntrypointFunction,
      @NonNull AssetManager assetManager,
      @Nullable List<String> entrypointArgs,
      long engineId) {
    ensureRunningOnMainThread();
    ensureAttachedToNative();
    nativeRunBundleAndSnapshotFromLibrary(
        nativeShellHolderId,
        bundlePath,
        entrypointFunctionName,
        pathToEntrypointFunction,
        assetManager,
        entrypointArgs,
        engineId);
  }

  private native void nativeRunBundleAndSnapshotFromLibrary(
      long nativeShellHolderId,
      @NonNull String bundlePath,
      @Nullable String entrypointFunctionName,
      @Nullable String pathToEntrypointFunction,
      @NonNull AssetManager manager,
      @Nullable List<String> entrypointArgs,
      long engineId);
  // ------ End Dart Execution Support -------

  // --------- Start Platform Message Support ------
  /**
   * Sets the handler for all platform messages that come from the attached
   * platform view to Java.
   *
   * <p>
   * Communication between a specific Flutter context (Dart) and the host platform
   * (Java) is
   * accomplished by passing messages. Messages can be sent from Java to Dart with
   * the corresponding
   * {@code FlutterJNI} methods:
   *
   * <ul>
   * <li>{@link #dispatchPlatformMessage(String, ByteBuffer, int, int)}
   * <li>{@link #dispatchEmptyPlatformMessage(String, int)}
   * </ul>
   *
   * <p>
   * {@code FlutterJNI} is also the recipient of all platform messages sent from
   * its attached
   * Flutter context. {@code FlutterJNI} does not know what to do with these
   * messages, so a handler
   * is exposed to allow these messages to be processed in whatever manner is
   * desired:
   *
   * <p>
   * {@code setPlatformMessageHandler(PlatformMessageHandler)}
   *
   * <p>
   * If a message is received but no {@link PlatformMessageHandler} is registered,
   * that message
   * will be dropped (ignored). Therefore, when using {@code FlutterJNI} to
   * integrate a Flutter
   * context in an app, a {@link PlatformMessageHandler} must be registered for
   * 2-way Java/Dart
   * communication to operate correctly. Moreover, the handler must be implemented
   * such that
   * fundamental platform messages are handled as expected.
   */
  @UiThread
  public void setPlatformMessageHandler(@Nullable PlatformMessageHandler platformMessageHandler) {
    ensureRunningOnMainThread();
    this.platformMessageHandler = platformMessageHandler;
  }

  private native void nativeCleanupMessageData(long messageData);

  /**
   * Destroys the resources provided sent to `handlePlatformMessage`.
   *
   * <p>
   * This can be called on any thread.
   *
   * @param messageData the argument sent to handlePlatformMessage.
   */
  public void cleanupMessageData(long messageData) {
    // This doesn't rely on being attached like other methods.
    nativeCleanupMessageData(messageData);
  }

  // Called by native on any thread.
  @SuppressWarnings("unused")
  @VisibleForTesting
  public void handlePlatformMessage(
      @NonNull final String channel,
      ByteBuffer message,
      final int replyId,
      final long messageData) {
    if (platformMessageHandler != null) {
      platformMessageHandler.handleMessageFromDart(channel, message, replyId, messageData);
    } else {
      nativeCleanupMessageData(messageData);
    }
  }

  // Called by native to respond to a platform message that we sent.
  @SuppressWarnings("unused")
  private void handlePlatformMessageResponse(int replyId, ByteBuffer reply) {
    if (platformMessageHandler != null) {
      platformMessageHandler.handlePlatformMessageResponse(replyId, reply);
    }
  }

  /**
   * Sends an empty reply (identified by {@code responseId}) from Android to
   * Flutter over the given
   * {@code channel}.
   */
  @UiThread
  public void dispatchEmptyPlatformMessage(@NonNull String channel, int responseId) {
    ensureRunningOnMainThread();
    if (isAttached()) {
      nativeDispatchEmptyPlatformMessage(nativeShellHolderId, channel, responseId);
    } else {
      Log.w(
          TAG,
          "Tried to send a platform message to Flutter, but FlutterJNI was detached from native C++. Could not send. Channel: "
              + channel
              + ". Response ID: "
              + responseId);
    }
  }

  // Send an empty platform message to Dart.
  private native void nativeDispatchEmptyPlatformMessage(
      long nativeShellHolderId, @NonNull String channel, int responseId);

  /**
   * Sends a reply {@code message} from Android to Flutter over the given
   * {@code channel}.
   */
  @UiThread
  public void dispatchPlatformMessage(
      @NonNull String channel, @Nullable ByteBuffer message, int position, int responseId) {
    ensureRunningOnMainThread();
    if (isAttached()) {
      nativeDispatchPlatformMessage(nativeShellHolderId, channel, message, position, responseId);
    } else {
      Log.w(
          TAG,
          "Tried to send a platform message to Flutter, but FlutterJNI was detached from native C++. Could not send. Channel: "
              + channel
              + ". Response ID: "
              + responseId);
    }
  }

  // Send a data-carrying platform message to Dart.
  private native void nativeDispatchPlatformMessage(
      long nativeShellHolderId,
      @NonNull String channel,
      @Nullable ByteBuffer message,
      int position,
      int responseId);

  public void invokePlatformMessageEmptyResponseCallback(int responseId) {
    // Called on any thread.
    shellHolderLock.readLock().lock();
    try {
      if (isAttached()) {
        nativeInvokePlatformMessageEmptyResponseCallback(nativeShellHolderId, responseId);
      } else {
        Log.w(
            TAG,
            "Tried to send a platform message response, but FlutterJNI was detached from native C++. Could not send. Response ID: "
                + responseId);
      }
    } finally {
      shellHolderLock.readLock().unlock();
    }
  }

  // Send an empty response to a platform message received from Dart.
  private native void nativeInvokePlatformMessageEmptyResponseCallback(
      long nativeShellHolderId, int responseId);

  public void invokePlatformMessageResponseCallback(
      int responseId, @NonNull ByteBuffer message, int position) {
    // Called on any thread.
    if (!message.isDirect()) {
      throw new IllegalArgumentException("Expected a direct ByteBuffer.");
    }
    shellHolderLock.readLock().lock();
    try {
      if (isAttached()) {
        nativeInvokePlatformMessageResponseCallback(
            nativeShellHolderId, responseId, message, position);
      } else {
        Log.w(
            TAG,
            "Tried to send a platform message response, but FlutterJNI was detached from native C++. Could not send. Response ID: "
                + responseId);
      }
    } finally {
      shellHolderLock.readLock().unlock();
    }
  }

  // Send a data-carrying response to a platform message received from Dart.
  private native void nativeInvokePlatformMessageResponseCallback(
      long nativeShellHolderId, int responseId, @Nullable ByteBuffer message, int position);
  // ------- End Platform Message Support ----

  // ----- Start Engine Lifecycle Support ----
  /**
   * Adds the given {@code engineLifecycleListener} to be notified of Flutter
   * engine lifecycle
   * events, e.g., {@link EngineLifecycleListener#onPreEngineRestart()}.
   */
  @UiThread
  public void addEngineLifecycleListener(@NonNull EngineLifecycleListener engineLifecycleListener) {
    ensureRunningOnMainThread();
    engineLifecycleListeners.add(engineLifecycleListener);
  }

  /**
   * Removes the given {@code engineLifecycleListener}, which was previously added
   * using {@link
   * #addEngineLifecycleListener(EngineLifecycleListener)}.
   */
  @UiThread
  public void removeEngineLifecycleListener(
      @NonNull EngineLifecycleListener engineLifecycleListener) {
    ensureRunningOnMainThread();
    engineLifecycleListeners.remove(engineLifecycleListener);
  }

  // Called by native.
  @SuppressWarnings("unused")
  private void onPreEngineRestart() {
    for (EngineLifecycleListener listener : engineLifecycleListeners) {
      listener.onPreEngineRestart();
    }
  }

  // ----- End Engine Lifecycle Support ----

  // ----- Start Localization Support ----

  /**
   * Sets the localization plugin that is used in various localization methods.
   */
  @UiThread
  public void setLocalizationPlugin(@Nullable LocalizationPlugin localizationPlugin) {
    ensureRunningOnMainThread();
    this.localizationPlugin = localizationPlugin;
  }

  /**
   * Invoked by native to obtain the results of Android's locale resolution
   * algorithm.
   */
  @SuppressWarnings("unused")
  @VisibleForTesting
  public String[] computePlatformResolvedLocale(@NonNull String[] strings) {
    if (localizationPlugin == null) {
      return new String[0];
    }
    List<Locale> supportedLocales = new ArrayList<Locale>();
    final int localeDataLength = 3;
    for (int i = 0; i < strings.length; i += localeDataLength) {
      String languageCode = strings[i + 0];
      String countryCode = strings[i + 1];
      String scriptCode = strings[i + 2];
      // Convert to Locales via LocaleBuilder if available (API 21+) to include
      // scriptCode.
      Locale.Builder localeBuilder = new Locale.Builder();
      if (!languageCode.isEmpty()) {
        localeBuilder.setLanguage(languageCode);
      }
      if (!countryCode.isEmpty()) {
        localeBuilder.setRegion(countryCode);
      }
      if (!scriptCode.isEmpty()) {
        localeBuilder.setScript(scriptCode);
      }
      supportedLocales.add(localeBuilder.build());
    }

    Locale result = localizationPlugin.resolveNativeLocale(supportedLocales);

    if (result == null) {
      return new String[0];
    }
    String[] output = new String[localeDataLength];
    output[0] = result.getLanguage();
    output[1] = result.getCountry();
    output[2] = result.getScript();
    return output;
  }

  // ----- Start Deferred Components Support ----

  /**
   * Sets the deferred component manager that is used to download and install
   * split features.
   */
  @UiThread
  public void setDeferredComponentManager(
      @Nullable DeferredComponentManager deferredComponentManager) {
    ensureRunningOnMainThread();
    this.deferredComponentManager = deferredComponentManager;
    if (deferredComponentManager != null) {
      deferredComponentManager.setJNI(this);
    }
  }

  /**
   * Called by dart to request that a Dart deferred library corresponding to
   * loadingUnitId be
   * downloaded (if necessary) and loaded into the dart vm.
   *
   * <p>
   * This method delegates the task to DeferredComponentManager, which handles the
   * download and
   * loading of the dart library and any assets.
   *
   * @param loadingUnitId The loadingUnitId is assigned during compile time by
   *                      gen_snapshot and is
   *                      automatically retrieved when loadLibrary() is called on
   *                      a dart deferred library.
   */
  @SuppressWarnings("unused")
  @UiThread
  public void requestDartDeferredLibrary(int loadingUnitId) {
    if (deferredComponentManager != null) {
      deferredComponentManager.installDeferredComponent(loadingUnitId, null);
    } else {
      // TODO(garyq): Add link to setup/instructions guide wiki.
      Log.e(
          TAG,
          "No DeferredComponentManager found. Android setup must be completed before using split AOT deferred components.");
    }
  }

  /**
   * Searches each of the provided paths for a valid Dart shared library .so file
   * and resolves
   * symbols to load into the dart VM.
   *
   * <p>
   * Successful loading of the dart library completes the future returned by
   * loadLibrary() that
   * triggered the install/load process.
   *
   * @param loadingUnitId The loadingUnitId is assigned during compile time by
   *                      gen_snapshot and is
   *                      automatically retrieved when loadLibrary() is called on
   *                      a dart deferred library. This is
   *                      used to identify which Dart deferred library the
   *                      resolved correspond to.
   * @param searchPaths   An array of paths in which to look for valid dart shared
   *                      libraries. This
   *                      supports paths within zipped apks as long as the apks
   *                      are not compressed using the
   *                      `path/to/apk.apk!path/inside/apk/lib.so` format. Paths
   *                      will be tried first to last and ends
   *                      when a library is successfully found. When the found
   *                      library is invalid, no additional
   *                      paths will be attempted.
   */
  @UiThread
  public void loadDartDeferredLibrary(int loadingUnitId, @NonNull String[] searchPaths) {
    ensureRunningOnMainThread();
    ensureAttachedToNative();
    nativeLoadDartDeferredLibrary(nativeShellHolderId, loadingUnitId, searchPaths);
  }

  private native void nativeLoadDartDeferredLibrary(
      long nativeShellHolderId, int loadingUnitId, @NonNull String[] searchPaths);

  /**
   * Adds the specified AssetManager as an APKAssetResolver in the Flutter
   * Engine's AssetManager.
   *
   * <p>
   * This may be used to update the engine AssetManager when a new deferred
   * component is
   * installed and a new Android AssetManager is created with access to new
   * assets.
   *
   * @param assetManager    An android AssetManager that is able to access the
   *                        newly downloaded assets.
   * @param assetBundlePath The subdirectory that the flutter assets are stored
   *                        in. The typical
   *                        value is `flutter_assets`.
   */
  @UiThread
  public void updateJavaAssetManager(
      @NonNull AssetManager assetManager, @NonNull String assetBundlePath) {
    ensureRunningOnMainThread();
    ensureAttachedToNative();
    nativeUpdateJavaAssetManager(nativeShellHolderId, assetManager, assetBundlePath);
  }

  private native void nativeUpdateJavaAssetManager(
      long nativeShellHolderId,
      @NonNull AssetManager assetManager,
      @NonNull String assetBundlePath);

  /**
   * Indicates that a failure was encountered during the Android portion of
   * downloading a dynamic
   * feature module and loading a dart deferred library, which is typically done
   * by
   * DeferredComponentManager.
   *
   * <p>
   * This will inform dart that the future returned by loadLibrary() should
   * complete with an
   * error.
   *
   * @param loadingUnitId The loadingUnitId that corresponds to the dart deferred
   *                      library that
   *                      failed to install.
   * @param error         The error message to display.
   * @param isTransient   When isTransient is false, new attempts to install will
   *                      automatically result
   *                      in same error in Dart before the request is passed to
   *                      Android.
   */
  @SuppressWarnings("unused")
  @UiThread
  public void deferredComponentInstallFailure(
      int loadingUnitId, @NonNull String error, boolean isTransient) {
    ensureRunningOnMainThread();
    nativeDeferredComponentInstallFailure(loadingUnitId, error, isTransient);
  }

  private native void nativeDeferredComponentInstallFailure(
      int loadingUnitId, @NonNull String error, boolean isTransient);

  // ----- End Deferred Components Support ----

  /**
   * Notifies the Dart VM of a low memory event, or that the application is in a
   * state such that now
   * is an appropriate time to free resources, such as going to the background.
   *
   * <p>
   * This is distinct from sending a SystemChannel message about low memory, which
   * only notifies
   * the running Flutter application.
   */
  @UiThread
  public void notifyLowMemoryWarning() {
    ensureRunningOnMainThread();
    ensureAttachedToNative();
    nativeNotifyLowMemoryWarning(nativeShellHolderId);
  }

  private native void nativeNotifyLowMemoryWarning(long nativeShellHolderId);

  private void ensureRunningOnMainThread() {
    if (Looper.myLooper() != mainLooper) {
      throw new RuntimeException(
          "Methods marked with @UiThread must be executed on the main thread. Current thread: "
              + Thread.currentThread().getName());
    }
  }
}
