// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_WINDOW_PLATFORM_CONFIGURATION_H_
#define FLUTTER_LIB_UI_WINDOW_PLATFORM_CONFIGURATION_H_

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "flutter/assets/asset_manager.h"
#include "flutter/lib/ui/window/platform_message_response.h"
#include "fml/macros.h"
#include "third_party/tonic/dart_persistent_value.h"
#include "third_party/tonic/typed_data/dart_byte_data.h"

namespace flutter {
class FontCollection;
class PlatformMessage;
class PlatformMessageHandler;
class PlatformIsolateManager;
class Scene;

// Forward declaration of friendly tests.
namespace testing {
FML_TEST_CLASS(PlatformConfigurationTest, BeginFrameMonotonic);
}

//--------------------------------------------------------------------------
/// @brief An enum for defining the different kinds of accessibility features
///        that can be enabled by the platform.
///
///         Must match the `AccessibilityFeatures` class in framework.
enum class AccessibilityFeatureFlag : int32_t {
  kAccessibleNavigation = 1 << 0,
  kInvertColors = 1 << 1,
  kDisableAnimations = 1 << 2,
  kBoldText = 1 << 3,
  kReduceMotion = 1 << 4,
  kHighContrast = 1 << 5,
  kOnOffSwitchLabels = 1 << 6,
  kNoAnnounce = 1 << 7,
};

//--------------------------------------------------------------------------
/// @brief A client interface that the `RuntimeController` uses to define
///        handlers for `PlatformConfiguration` requests.
///
/// @see   `PlatformConfiguration`
///
class PlatformConfigurationClient {
 public:
  //--------------------------------------------------------------------------
  /// @brief      When the Flutter application has a message to send to the
  ///             underlying platform, the message needs to be forwarded to
  ///             the platform on the appropriate thread (via the platform
  ///             task runner). The PlatformConfiguration delegates this task
  ///             to the engine via this method.
  ///
  /// @see        `PlatformView::HandlePlatformMessage`
  ///
  /// @param[in]  message  The message from the Flutter application to send to
  ///                      the underlying platform.
  ///
  virtual void HandlePlatformMessage(
      std::unique_ptr<PlatformMessage> message) = 0;

  //--------------------------------------------------------------------------
  /// @brief      Returns the current collection of assets available on the
  ///             platform.
  virtual std::shared_ptr<AssetManager> GetAssetManager() = 0;

  //--------------------------------------------------------------------------
  /// @brief      Notifies this client of the name of the root isolate and its
  ///             port when that isolate is launched, restarted (in the
  ///             cold-restart scenario) or the application itself updates the
  ///             name of the root isolate (via `Window.setIsolateDebugName`
  ///             in `window.dart`). The name of the isolate is meaningless to
  ///             the engine but is used in instrumentation and tooling.
  ///             Currently, this information is to update the service
  ///             protocol list of available root isolates running in the VM
  ///             and their names so that the appropriate isolate can be
  ///             selected in the tools for debugging and instrumentation.
  ///
  /// @param[in]  isolate_name  The isolate name
  /// @param[in]  isolate_port  The isolate port
  ///
  virtual void UpdateIsolateDescription(const std::string isolate_name,
                                        int64_t isolate_port) = 0;

  //--------------------------------------------------------------------------
  /// @brief      The embedder can specify data that the isolate can request
  ///             synchronously on launch. This accessor fetches that data.
  ///
  ///             This data is persistent for the duration of the Flutter
  ///             application and is available even after isolate restarts.
  ///             Because of this lifecycle, the size of this data must be kept
  ///             to a minimum.
  ///
  ///             For asynchronous communication between the embedder and
  ///             isolate, a platform channel may be used.
  ///
  /// @return     A map of the isolate data that the framework can request upon
  ///             launch.
  ///
  virtual std::shared_ptr<const fml::Mapping> GetPersistentIsolateData() = 0;

  //--------------------------------------------------------------------------
  /// @brief      Directly invokes platform-specific APIs to compute the
  ///             locale the platform would have natively resolved to.
  ///
  /// @param[in]  supported_locale_data  The vector of strings that represents
  ///                                    the locales supported by the app.
  ///                                    Each locale consists of three
  ///                                    strings: languageCode, countryCode,
  ///                                    and scriptCode in that order.
  ///
  /// @return     A vector of 3 strings languageCode, countryCode, and
  ///             scriptCode that represents the locale selected by the
  ///             platform. Empty strings mean the value was unassigned. Empty
  ///             vector represents a null locale.
  ///
  virtual std::unique_ptr<std::vector<std::string>>
  ComputePlatformResolvedLocale(
      const std::vector<std::string>& supported_locale_data) = 0;

  //--------------------------------------------------------------------------
  /// @brief      Invoked when the Dart VM requests that a deferred library
  ///             be loaded. Notifies the engine that the deferred library
  ///             identified by the specified loading unit id should be
  ///             downloaded and loaded into the Dart VM via
  ///             `LoadDartDeferredLibrary`
  ///
  ///             Upon encountering errors or otherwise failing to load a
  ///             loading unit with the specified id, the failure should be
  ///             directly reported to dart by calling
  ///             `LoadDartDeferredLibraryFailure` to ensure the waiting dart
  ///             future completes with an error.
  ///
  /// @param[in]  loading_unit_id  The unique id of the deferred library's
  ///                              loading unit. This id is to be passed
  ///                              back into LoadDartDeferredLibrary
  ///                              in order to identify which deferred
  ///                              library to load.
  ///
  virtual void RequestDartDeferredLibrary(intptr_t loading_unit_id) = 0;

  //--------------------------------------------------------------------------
  /// @brief      Invoked when a listener is registered on a platform channel.
  ///
  /// @param[in]  name             The name of the platform channel to which a
  ///                              listener has been registered or cleared.
  ///
  /// @param[in]  listening        Whether the listener has been set (true) or
  ///                              cleared (false).
  ///
  virtual void SendChannelUpdate(std::string name, bool listening) = 0;

  virtual std::shared_ptr<PlatformIsolateManager>
  GetPlatformIsolateManager() = 0;

 protected:
  virtual ~PlatformConfigurationClient();
};

//----------------------------------------------------------------------------
/// @brief      A class for holding and distributing platform-level information
///             to and from the Dart code in Flutter's framework.
///
///             It handles communication between the engine and the framework.
///
///             It communicates with the RuntimeController through the use of a
///             PlatformConfigurationClient interface, which the
///             RuntimeController defines.
///
class PlatformConfiguration final {
 public:
  //----------------------------------------------------------------------------
  /// @brief      Creates a new PlatformConfiguration, typically created by the
  ///             RuntimeController.
  ///
  /// @param[in] client The `PlatformConfigurationClient` to be injected into
  ///                   the PlatformConfiguration. This client is used to
  ///                   forward requests to the RuntimeController.
  ///
  explicit PlatformConfiguration(PlatformConfigurationClient* client);

  // PlatformConfiguration is not copyable.
  PlatformConfiguration(const PlatformConfiguration&) = delete;
  PlatformConfiguration& operator=(const PlatformConfiguration&) = delete;

  ~PlatformConfiguration();

  //----------------------------------------------------------------------------
  /// @brief      Access to the platform configuration client (which typically
  ///             is implemented by the RuntimeController).
  ///
  /// @return     Returns the client used to construct this
  /// PlatformConfiguration.
  ///
  PlatformConfigurationClient* client() const { return client_; }

  //----------------------------------------------------------------------------
  /// @brief      Called by the RuntimeController once it has created the root
  ///             isolate, so that the PlatformController can get a handle to
  ///             the 'dart:ui' library.
  ///
  ///             It uses the handle to call the hooks in hooks.dart.
  ///
  void DidCreateIsolate();

  /// @brief     Sets the opaque identifier of the engine.
  ///
  ///            The identifier can be passed from Dart to native code to
  ///            retrieve the engine instance.
  ///
  /// @return    Whether the identifier was set.
  bool SetEngineId(int64_t engine_id);

  //----------------------------------------------------------------------------
  /// @brief      Update the specified locale data in the framework.
  ///
  /// @param[in]  locale_data  The locale data. This should consist of groups of
  ///             4 strings, each group representing a single locale.
  ///
  void UpdateLocales(const std::vector<std::string>& locales);

  //----------------------------------------------------------------------------
  /// @brief      Notifies the PlatformConfiguration that the client has sent
  ///             it a message. This call originates in the platform view and
  ///             has been forwarded through the engine to here.
  ///
  /// @param[in]  message  The message sent from the embedder to the Dart
  ///                      application.
  ///
  void DispatchPlatformMessage(std::unique_ptr<PlatformMessage> message);

  //----------------------------------------------------------------------------
  /// @brief      Responds to a previous platform message to the engine from the
  ///             framework.
  ///
  /// @param[in] response_id The unique id that identifies the original platform
  ///                        message to respond to.
  /// @param[in] data        The data to send back in the response.
  ///
  void CompletePlatformMessageResponse(int response_id,
                                       std::vector<uint8_t> data);

  //----------------------------------------------------------------------------
  /// @brief      Responds to a previous platform message to the engine from the
  ///             framework with an empty response.
  ///
  /// @param[in] response_id The unique id that identifies the original platform
  ///                        message to respond to.
  ///
  void CompletePlatformMessageEmptyResponse(int response_id);

  Dart_Handle on_error() { return on_error_.Get(); }

 private:
  FML_FRIEND_TEST(testing::PlatformConfigurationTest, BeginFrameMonotonic);

  PlatformConfigurationClient* client_;
  tonic::DartPersistentValue on_error_;
  tonic::DartPersistentValue set_engine_id_;
  tonic::DartPersistentValue update_locales_;
  tonic::DartPersistentValue dispatch_platform_message_;

  // ID starts at 1 because an ID of 0 indicates that no response is expected.
  int next_response_id_ = 1;
  std::unordered_map<int, fml::RefPtr<PlatformMessageResponse>>
      pending_responses_;
};

//----------------------------------------------------------------------------
/// An inteface that the result of `Dart_CurrentIsolateGroupData` should
/// implement for registering background isolates to work.
class PlatformMessageHandlerStorage {
 public:
  virtual ~PlatformMessageHandlerStorage() = default;
  virtual void SetPlatformMessageHandler(
      int64_t root_isolate_token,
      std::weak_ptr<PlatformMessageHandler> handler) = 0;

  virtual std::weak_ptr<PlatformMessageHandler> GetPlatformMessageHandler(
      int64_t root_isolate_token) const = 0;
};

//----------------------------------------------------------------------------
// API exposed as FFI calls in Dart.
//
// These are probably not supposed to be called directly, and should instead
// be called through their sibling API in `PlatformConfiguration` or
// `PlatformConfigurationClient`.
//
// These are intentionally undocumented. Refer instead to the sibling methods
// above.
//----------------------------------------------------------------------------
class PlatformConfigurationNativeApi {
 public:
  static Dart_Handle GetPersistentIsolateData();

  static Dart_Handle ComputePlatformResolvedLocale(
      Dart_Handle supportedLocalesHandle);

  static void SetIsolateDebugName(const std::string& name);

  static Dart_Handle SendPlatformMessage(const std::string& name,
                                         Dart_Handle callback,
                                         Dart_Handle data_handle);

  static Dart_Handle SendPortPlatformMessage(const std::string& name,
                                             Dart_Handle identifier,
                                             Dart_Handle send_port,
                                             Dart_Handle data_handle);

  static void RespondToPlatformMessage(int response_id,
                                       const tonic::DartByteData& data);

  static void SendChannelUpdate(const std::string& name, bool listening);

  //--------------------------------------------------------------------------
  /// @brief      Requests the Dart VM to adjusts the GC heuristics based on
  ///             the requested `performance_mode`. Returns the old performance
  ///             mode.
  ///
  ///             Requesting a performance mode doesn't guarantee any
  ///             performance characteristics. This is best effort, and should
  ///             be used after careful consideration of the various GC
  ///             trade-offs.
  ///
  /// @param[in]  performance_mode The requested performance mode. Please refer
  ///                              to documentation of `Dart_PerformanceMode`
  ///                              for more details about what each performance
  ///                              mode does.
  ///
  static int RequestDartPerformanceMode(int mode);

  //--------------------------------------------------------------------------
  /// @brief      Returns the current performance mode of the Dart VM. Defaults
  /// to `Dart_PerformanceMode_Default` if no prior requests to change the
  /// performance mode have been made.
  static Dart_PerformanceMode GetDartPerformanceMode();

  static int64_t GetRootIsolateToken();

  static void RegisterBackgroundIsolate(int64_t root_isolate_token);

 private:
  static Dart_PerformanceMode current_performance_mode_;
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_WINDOW_PLATFORM_CONFIGURATION_H_
