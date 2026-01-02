// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import static org.junit.Assert.*;
import static org.mockito.Mockito.*;

import android.content.Context;
import androidx.annotation.NonNull;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import io.flutter.embedding.engine.loader.FlutterLoader;
import io.flutter.embedding.engine.plugins.FlutterPlugin;
import org.junit.Test;
import org.junit.runner.RunWith;

// Run with Robolectric so that Log calls don't crash.

@RunWith(AndroidJUnit4.class)
public class FlutterEngineConnectionRegistryTest {
  @Test
  public void itDoesNotRegisterTheSamePluginTwice() {
    Context context = mock(Context.class);

    FlutterEngine flutterEngine = mock(FlutterEngine.class);
    FlutterLoader flutterLoader = mock(FlutterLoader.class);

    FakeFlutterPlugin fakePlugin1 = new FakeFlutterPlugin();
    FakeFlutterPlugin fakePlugin2 = new FakeFlutterPlugin();

    FlutterEngineConnectionRegistry registry =
        new FlutterEngineConnectionRegistry(context, flutterEngine, flutterLoader, null);

    // Verify that the registry doesn't think it contains our plugin yet.
    assertFalse(registry.has(fakePlugin1.getClass()));

    // Add our plugin to the registry.
    registry.add(fakePlugin1);

    // Verify that the registry now thinks it contains our plugin.
    assertTrue(registry.has(fakePlugin1.getClass()));
    assertEquals(1, fakePlugin1.attachmentCallCount);

    // Add a different instance of the same plugin class.
    registry.add(fakePlugin2);

    // Verify that the registry did not detach the 1st plugin, and
    // it did not attach the 2nd plugin.
    assertEquals(1, fakePlugin1.attachmentCallCount);
    assertEquals(0, fakePlugin1.detachmentCallCount);

    assertEquals(0, fakePlugin2.attachmentCallCount);
    assertEquals(0, fakePlugin2.detachmentCallCount);
  }

  private static class FakeFlutterPlugin implements FlutterPlugin {
    public int attachmentCallCount = 0;
    public int detachmentCallCount = 0;

    @Override
    public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
      attachmentCallCount += 1;
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
      detachmentCallCount += 1;
    }
  }
}
