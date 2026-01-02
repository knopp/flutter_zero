// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/testing/mock_plugin_registrar.h"

struct _FlMockPluginRegistrar {
  GObject parent_instance;

  FlBinaryMessenger* messenger;
};

static void fl_mock_plugin_registrar_iface_init(
    FlPluginRegistrarInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    FlMockPluginRegistrar,
    fl_mock_plugin_registrar,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(fl_plugin_registrar_get_type(),
                          fl_mock_plugin_registrar_iface_init))

static void fl_mock_plugin_registrar_dispose(GObject* object) {
  FlMockPluginRegistrar* self = FL_MOCK_PLUGIN_REGISTRAR(object);

  g_clear_object(&self->messenger);

  G_OBJECT_CLASS(fl_mock_plugin_registrar_parent_class)->dispose(object);
}

static void fl_mock_plugin_registrar_class_init(
    FlMockPluginRegistrarClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_mock_plugin_registrar_dispose;
}

static FlBinaryMessenger* get_messenger(FlPluginRegistrar* registrar) {
  FlMockPluginRegistrar* self = FL_MOCK_PLUGIN_REGISTRAR(registrar);
  return self->messenger;
}

static void fl_mock_plugin_registrar_iface_init(
    FlPluginRegistrarInterface* iface) {
  iface->get_messenger = get_messenger;
}

static void fl_mock_plugin_registrar_init(FlMockPluginRegistrar* self) {}

FlPluginRegistrar* fl_mock_plugin_registrar_new(FlBinaryMessenger* messenger) {
  FlMockPluginRegistrar* registrar = FL_MOCK_PLUGIN_REGISTRAR(
      g_object_new(fl_mock_plugin_registrar_get_type(), NULL));
  registrar->messenger = FL_BINARY_MESSENGER(g_object_ref(messenger));
  return FL_PLUGIN_REGISTRAR(registrar);
}
