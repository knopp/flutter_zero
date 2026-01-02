// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_COMMON_PLATFORM_VIEW_H_
#define FLUTTER_SHELL_COMMON_PLATFORM_VIEW_H_

#include <memory>

#include "flutter/assets/asset_resolver.h"
#include "flutter/common/settings.h"
#include "flutter/common/task_runners.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/mapping.h"
#include "flutter/fml/memory/weak_ptr.h"
#include "flutter/lib/ui/window/platform_message.h"
#include "flutter/shell/common/platform_message_handler.h"

namespace flutter {

//------------------------------------------------------------------------------
/// @brief      Platform views are created by the shell on the platform task
///             runner. Unless explicitly specified, all platform view methods
///             are called on the platform task runner as well. Platform views
///             are usually sub-classed on a per platform basis and the bulk of
///             the window system integration happens using that subclass. Since
///             most platform window toolkits are usually only safe to access on
///             a single "main" thread, any interaction that requires access to
///             the underlying platform's window toolkit is routed through the
///             platform view associated with that shell. This involves
///             operations like settings up and tearing down the render surface,
///             platform messages, interacting with accessibility features on
///             the platform, input events, etc.
///
class PlatformView {
 public:
  //----------------------------------------------------------------------------
  /// @brief      Used to forward events from the platform view to interested
  ///             subsystems. This forwarding is done by the shell which sets
  ///             itself up as the delegate of the platform view.
  ///
  class Delegate {
   public:
    //--------------------------------------------------------------------------
    /// @brief      Notifies the delegate that the platform view was created.
    ///
    virtual void OnPlatformViewCreated() = 0;

    //--------------------------------------------------------------------------
    /// @brief      Notifies the delegate that the platform view was destroyed.
    ///             This is usually a sign to the rasterizer to suspend
    ///             rendering a previously configured surface and collect any
    ///             intermediate resources.
    ///
    virtual void OnPlatformViewDestroyed() = 0;

    //--------------------------------------------------------------------------
    /// @brief      Notifies the delegate that the platform has dispatched a
    ///             platform message from the embedder to the Flutter
    ///             application. This message must be forwarded to the running
    ///             isolate hosted by the engine on the UI thread.
    ///
    /// @param[in]  message  The platform message to dispatch to the running
    ///                      root isolate.
    ///
    virtual void OnPlatformViewDispatchPlatformMessage(
        std::unique_ptr<PlatformMessage> message) = 0;

    //--------------------------------------------------------------------------
    /// @brief      Loads the dart shared library into the dart VM. When the
    ///             dart library is loaded successfully, the dart future
    ///             returned by the originating loadLibrary() call completes.
    ///
    ///             The Dart compiler may generate separate shared libraries
    ///             files called 'loading units' when libraries are imported
    ///             as deferred. Each of these shared libraries are identified
    ///             by a unique loading unit id. Callers should open and resolve
    ///             a SymbolMapping from the shared library. The Mappings should
    ///             be moved into this method, as ownership will be assumed by
    ///             the dart root isolate after successful loading and released
    ///             after shutdown of the root isolate. The loading unit may not
    ///             be used after isolate shutdown. If loading fails, the
    ///             mappings will be released.
    ///
    ///             This method is paired with a RequestDartDeferredLibrary
    ///             invocation that provides the embedder with the loading unit
    ///             id of the deferred library to load.
    ///
    ///
    /// @param[in]  loading_unit_id  The unique id of the deferred library's
    ///                              loading unit.
    ///
    /// @param[in]  snapshot_data    Dart snapshot data of the loading unit's
    ///                              shared library.
    ///
    /// @param[in]  snapshot_data    Dart snapshot instructions of the loading
    ///                              unit's shared library.
    ///
    virtual void LoadDartDeferredLibrary(
        intptr_t loading_unit_id,
        std::unique_ptr<const fml::Mapping> snapshot_data,
        std::unique_ptr<const fml::Mapping> snapshot_instructions) = 0;

    //--------------------------------------------------------------------------
    /// @brief      Indicates to the dart VM that the request to load a deferred
    ///             library with the specified loading unit id has failed.
    ///
    ///             The dart future returned by the initiating loadLibrary()
    ///             call will complete with an error.
    ///
    /// @param[in]  loading_unit_id  The unique id of the deferred library's
    ///                              loading unit, as passed in by
    ///                              RequestDartDeferredLibrary.
    ///
    /// @param[in]  error_message    The error message that will appear in the
    ///                              dart Future.
    ///
    /// @param[in]  transient        A transient error is a failure due to
    ///                              temporary conditions such as no network.
    ///                              Transient errors allow the dart VM to
    ///                              re-request the same deferred library and
    ///                              loading_unit_id again. Non-transient
    ///                              errors are permanent and attempts to
    ///                              re-request the library will instantly
    ///                              complete with an error.
    virtual void LoadDartDeferredLibraryError(intptr_t loading_unit_id,
                                              const std::string error_message,
                                              bool transient) = 0;

    //--------------------------------------------------------------------------
    /// @brief      Replaces the asset resolver handled by the engine's
    ///             AssetManager of the specified `type` with
    ///             `updated_asset_resolver`. The matching AssetResolver is
    ///             removed and replaced with `updated_asset_resolvers`.
    ///
    ///             AssetResolvers should be updated when the existing resolver
    ///             becomes obsolete and a newer one becomes available that
    ///             provides updated access to the same type of assets as the
    ///             existing one. This update process is meant to be performed
    ///             at runtime.
    ///
    ///             If a null resolver is provided, nothing will be done. If no
    ///             matching resolver is found, the provided resolver will be
    ///             added to the end of the AssetManager resolvers queue. The
    ///             replacement only occurs with the first matching resolver.
    ///             Any additional matching resolvers are untouched.
    ///
    /// @param[in]  updated_asset_resolver  The asset resolver to replace the
    ///             resolver of matching type with.
    ///
    /// @param[in]  type  The type of AssetResolver to update. Only resolvers of
    ///                   the specified type will be replaced by the updated
    ///                   resolver.
    ///
    virtual void UpdateAssetResolverByType(
        std::unique_ptr<AssetResolver> updated_asset_resolver,
        AssetResolver::AssetResolverType type) = 0;

    //--------------------------------------------------------------------------
    /// @brief      Called by the platform view on the platform thread to get
    ///             the settings object associated with the platform view
    ///             instance.
    ///
    /// @return     The settings.
    ///
    virtual const Settings& OnPlatformViewGetSettings() const = 0;
  };

  //----------------------------------------------------------------------------
  /// @brief      Creates a platform view with the specified delegate and task
  ///             runner. The base class by itself does not do much but is
  ///             suitable for use in test environments where full platform
  ///             integration may not be necessary. The platform view may only
  ///             be created, accessed and destroyed on the platform task
  ///             runner.
  ///
  /// @param      delegate      The delegate. This is typically the shell.
  /// @param[in]  task_runners  The task runners used by this platform view.
  ///
  explicit PlatformView(Delegate& delegate, const TaskRunners& task_runners);

  //----------------------------------------------------------------------------
  /// @brief      Destroys the platform view. The platform view is owned by the
  ///             shell and will be destroyed by the same on the platform tasks
  ///             runner.
  ///
  virtual ~PlatformView();

  //----------------------------------------------------------------------------
  /// @brief      Used by embedders to dispatch a platform message to a
  ///             running root isolate hosted by the engine. If an isolate is
  ///             not running, the message is dropped. If there is no one on the
  ///             other side listening on the channel, the message is dropped.
  ///             When a platform message is dropped, any response handles
  ///             associated with that message will be dropped as well. All
  ///             users of platform messages must assume that message may not be
  ///             delivered and/or their response handles may not be invoked.
  ///             Platform messages are not buffered.
  ///
  ///             For embedders that wish to respond to platform message
  ///             directed from the framework to the embedder, the
  ///             `HandlePlatformMessage` method may be overridden.
  ///
  /// @see        HandlePlatformMessage()
  ///
  /// @param[in]  message  The platform message to deliver to the root isolate.
  ///
  void DispatchPlatformMessage(std::unique_ptr<PlatformMessage> message);

  //----------------------------------------------------------------------------
  /// @brief      Overridden by embedders to perform actions in response to
  ///             platform messages sent from the framework to the embedder.
  ///             Default implementation of this method simply returns an empty
  ///             response.
  ///
  ///             Embedders that wish to send platform messages to the framework
  ///             may use the `DispatchPlatformMessage` method. This method is
  ///             for messages that go the other way.
  ///
  /// @see        DispatchPlatformMessage()
  ///
  /// @param[in]  message  The message
  ///
  virtual void HandlePlatformMessage(std::unique_ptr<PlatformMessage> message);

  //----------------------------------------------------------------------------
  /// @brief      Used by the framework to tell the embedder that it has
  ///             registered a listener on a given channel.
  ///
  /// @param[in]  name      The name of the channel on which the listener has
  ///                       set or cleared a listener.
  /// @param[in]  listening True if a listener has been set, false if it has
  ///                       been cleared.
  ///
  virtual void SendChannelUpdate(const std::string& name, bool listening);

  //----------------------------------------------------------------------------
  /// @brief      Used by embedders to notify the shell that a platform view
  ///             has been created. This notification is used to create a
  ///             rendering surface and pick the client rendering API to use to
  ///             render into this surface. No frames will be scheduled or
  ///             rendered before this call. The surface must remain valid till
  ///             the corresponding call to NotifyDestroyed.
  ///
  void NotifyCreated();

  //----------------------------------------------------------------------------
  /// @brief      Used by embedders to notify the shell that the platform view
  ///             has been destroyed. This notification used to collect the
  ///             rendering surface and all associated resources. Frame
  ///             scheduling is also suspended.
  ///
  /// @attention  Subclasses may choose to override this method to perform
  ///             platform specific functions. However, they must call the base
  ///             class method at some point in their implementation.
  ///
  virtual void NotifyDestroyed();

  //----------------------------------------------------------------------------
  /// @brief      Returns a weak pointer to the platform view. Since the
  ///             platform view may only be created, accessed and destroyed
  ///             on the platform thread, any access to the platform view
  ///             from a non-platform task runner needs a weak pointer to
  ///             the platform view along with a reference to the platform
  ///             task runner. A task must be posted to the platform task
  ///             runner with the weak pointer captured in the same. The
  ///             platform view method may only be called in the posted task
  ///             once the weak pointer validity has been checked. This
  ///             method is used by callers to obtain that weak pointer.
  ///
  /// @return     The weak pointer to the platform view.
  ///
  fml::WeakPtr<PlatformView> GetWeakPtr() const;

  //----------------------------------------------------------------------------
  /// @brief      Gives embedders a chance to react to a "cold restart" of the
  ///             running isolate. The default implementation of this method
  ///             does nothing.
  ///
  ///             While a "hot restart" patches a running isolate, a "cold
  ///             restart" restarts the root isolate in a running shell.
  ///
  virtual void OnPreEngineRestart() const;

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
  ComputePlatformResolvedLocales(
      const std::vector<std::string>& supported_locale_data);

  //--------------------------------------------------------------------------
  /// @brief      Invoked when the dart VM requests that a deferred library
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
  virtual void RequestDartDeferredLibrary(intptr_t loading_unit_id);

  //--------------------------------------------------------------------------
  /// @brief      Loads the Dart shared library into the Dart VM. When the
  ///             Dart library is loaded successfully, the Dart future
  ///             returned by the originating loadLibrary() call completes.
  ///
  ///             The Dart compiler may generate separate shared libraries
  ///             files called 'loading units' when libraries are imported
  ///             as deferred. Each of these shared libraries are identified
  ///             by a unique loading unit id. Callers should open and resolve
  ///             a SymbolMapping from the shared library. The Mappings should
  ///             be moved into this method, as ownership will be assumed by the
  ///             dart isolate after successful loading and released after
  ///             shutdown of the dart isolate. If loading fails, the mappings
  ///             will naturally go out of scope.
  ///
  ///             This method is paired with a RequestDartDeferredLibrary
  ///             invocation that provides the embedder with the loading unit id
  ///             of the deferred library to load.
  ///
  ///
  /// @param[in]  loading_unit_id  The unique id of the deferred library's
  ///                              loading unit, as passed in by
  ///                              RequestDartDeferredLibrary.
  ///
  /// @param[in]  snapshot_data    Dart snapshot data of the loading unit's
  ///                              shared library.
  ///
  /// @param[in]  snapshot_data    Dart snapshot instructions of the loading
  ///                              unit's shared library.
  ///
  virtual void LoadDartDeferredLibrary(
      intptr_t loading_unit_id,
      std::unique_ptr<const fml::Mapping> snapshot_data,
      std::unique_ptr<const fml::Mapping> snapshot_instructions);

  //--------------------------------------------------------------------------
  /// @brief      Indicates to the dart VM that the request to load a deferred
  ///             library with the specified loading unit id has failed.
  ///
  ///             The dart future returned by the initiating loadLibrary() call
  ///             will complete with an error.
  ///
  /// @param[in]  loading_unit_id  The unique id of the deferred library's
  ///                              loading unit, as passed in by
  ///                              RequestDartDeferredLibrary.
  ///
  /// @param[in]  error_message    The error message that will appear in the
  ///                              dart Future.
  ///
  /// @param[in]  transient        A transient error is a failure due to
  ///                              temporary conditions such as no network.
  ///                              Transient errors allow the dart VM to
  ///                              re-request the same deferred library and
  ///                              loading_unit_id again. Non-transient
  ///                              errors are permanent and attempts to
  ///                              re-request the library will instantly
  ///                              complete with an error.
  ///
  virtual void LoadDartDeferredLibraryError(intptr_t loading_unit_id,
                                            const std::string error_message,
                                            bool transient);

  //--------------------------------------------------------------------------
  /// @brief      Replaces the asset resolver handled by the engine's
  ///             AssetManager of the specified `type` with
  ///             `updated_asset_resolver`. The matching AssetResolver is
  ///             removed and replaced with `updated_asset_resolvers`.
  ///
  ///             AssetResolvers should be updated when the existing resolver
  ///             becomes obsolete and a newer one becomes available that
  ///             provides updated access to the same type of assets as the
  ///             existing one. This update process is meant to be performed
  ///             at runtime.
  ///
  ///             If a null resolver is provided, nothing will be done. If no
  ///             matching resolver is found, the provided resolver will be
  ///             added to the end of the AssetManager resolvers queue. The
  ///             replacement only occurs with the first matching resolver.
  ///             Any additional matching resolvers are untouched.
  ///
  /// @param[in]  updated_asset_resolver  The asset resolver to replace the
  ///             resolver of matching type with.
  ///
  /// @param[in]  type  The type of AssetResolver to update. Only resolvers of
  ///                   the specified type will be replaced by the updated
  ///                   resolver.
  ///
  virtual void UpdateAssetResolverByType(
      std::unique_ptr<AssetResolver> updated_asset_resolver,
      AssetResolver::AssetResolverType type);

  //--------------------------------------------------------------------------
  /// @brief Specifies a delegate that will receive PlatformMessages from
  /// Flutter to the host platform.
  ///
  /// @details If this returns `null` that means PlatformMessages should be sent
  /// to the PlatformView.  That is to protect legacy behavior, any embedder
  /// that wants to support executing Platform Channel handlers on background
  /// threads should be returning a thread-safe PlatformMessageHandler instead.
  virtual std::shared_ptr<PlatformMessageHandler> GetPlatformMessageHandler()
      const;

  //----------------------------------------------------------------------------
  /// @brief      Get the settings for this platform view instance.
  ///
  /// @return     The settings.
  ///
  const Settings& GetSettings() const;

 protected:
  PlatformView::Delegate& delegate_;
  const TaskRunners task_runners_;
  fml::WeakPtrFactory<PlatformView> weak_factory_;  // Must be the last member.

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(PlatformView);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_COMMON_PLATFORM_VIEW_H_
