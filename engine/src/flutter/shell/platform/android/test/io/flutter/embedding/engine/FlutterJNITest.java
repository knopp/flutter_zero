// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package io.flutter.embedding.engine;

import static io.flutter.Build.API_LEVELS;
import static org.junit.Assert.assertEquals;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import android.annotation.TargetApi;
import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import io.flutter.embedding.engine.dart.DartExecutor;
import io.flutter.embedding.engine.systemchannels.LocalizationChannel;
import io.flutter.plugin.localization.LocalizationPlugin;
import java.nio.ByteBuffer;
import java.util.Locale;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
@TargetApi(API_LEVELS.API_24) // LocaleList and scriptCode are API 24+.
public class FlutterJNITest {
  @Test
  public void computePlatformResolvedLocaleCallsLocalizationPluginProperly() {
    // --- Test Setup ---
    FlutterJNI flutterJNI = new FlutterJNI();

    Context context = mock(Context.class);
    Resources resources = mock(Resources.class);
    Configuration config = mock(Configuration.class);
    DartExecutor dartExecutor = mock(DartExecutor.class);
    LocaleList localeList =
        new LocaleList(
            new Locale.Builder().setLanguage("es").setRegion("MX").build(),
            new Locale.Builder().setLanguage("zh").setRegion("CN").build(),
            new Locale.Builder().setLanguage("en").setRegion("US").build());
    when(context.getResources()).thenReturn(resources);
    when(resources.getConfiguration()).thenReturn(config);
    when(config.getLocales()).thenReturn(localeList);

    flutterJNI.setLocalizationPlugin(
        new LocalizationPlugin(context, new LocalizationChannel(dartExecutor)));
    String[] supportedLocales =
        new String[] {
          "fr", "FR", "",
          "zh", "", "",
          "en", "CA", ""
        };
    String[] result = flutterJNI.computePlatformResolvedLocale(supportedLocales);
    assertEquals(3, result.length);
    assertEquals("zh", result[0]);
    assertEquals("", result[1]);
    assertEquals("", result[2]);

    supportedLocales =
        new String[] {
          "fr", "FR", "",
          "ar", "", "",
          "en", "CA", ""
        };
    result = flutterJNI.computePlatformResolvedLocale(supportedLocales);
    assertEquals(3, result.length);
    assertEquals("en", result[0]);
    assertEquals("CA", result[1]);
    assertEquals("", result[2]);

    supportedLocales =
        new String[] {
          "fr", "FR", "",
          "ar", "", "",
          "en", "US", ""
        };
    result = flutterJNI.computePlatformResolvedLocale(supportedLocales);
    assertEquals(3, result.length);
    assertEquals("en", result[0]);
    assertEquals("US", result[1]);
    assertEquals("", result[2]);

    supportedLocales =
        new String[] {
          "ar", "", "",
          "es", "MX", "",
          "en", "US", ""
        };
    result = flutterJNI.computePlatformResolvedLocale(supportedLocales);
    assertEquals(3, result.length);
    assertEquals("es", result[0]);
    assertEquals("MX", result[1]);
    assertEquals("", result[2]);

    // Empty supportedLocales.
    supportedLocales = new String[] {};
    result = flutterJNI.computePlatformResolvedLocale(supportedLocales);
    assertEquals(0, result.length);

    // Empty preferredLocales.
    supportedLocales =
        new String[] {
          "fr", "FR", "",
          "zh", "", "",
          "en", "CA", ""
        };
    localeList = new LocaleList();
    when(config.getLocales()).thenReturn(localeList);
    result = flutterJNI.computePlatformResolvedLocale(supportedLocales);
    // The first locale is default.
    assertEquals(3, result.length);
    assertEquals("fr", result[0]);
    assertEquals("FR", result[1]);
    assertEquals("", result[2]);
  }

  @Test(expected = IllegalArgumentException.class)
  public void invokePlatformMessageResponseCallback_wantsDirectBuffer() {
    FlutterJNI flutterJNI = new FlutterJNI();
    ByteBuffer buffer = ByteBuffer.allocate(4);
    flutterJNI.invokePlatformMessageResponseCallback(0, buffer, buffer.position());
  }

  static class FlutterJNITester extends FlutterJNI {
    FlutterJNITester(boolean attached) {
      this.isAttached = attached;
    }

    final boolean isAttached;
    boolean semanticsEnabled = false;
    int flags = 0;

    @Override
    public boolean isAttached() {
      return isAttached;
    }
  }
}
