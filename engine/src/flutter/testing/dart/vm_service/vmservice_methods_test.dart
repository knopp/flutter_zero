// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:convert';
import 'dart:developer' as developer;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:test/test.dart';
import 'package:vm_service/vm_service.dart' as vms;
import 'package:vm_service/vm_service_io.dart';

void main() {
  test('Setting invalid directory returns an error', () async {
    vms.VmService? vmService;
    try {
      final developer.ServiceProtocolInfo info = await developer.Service.getInfo();
      if (info.serverUri == null) {
        fail('This test must not be run with --disable-vm-service.');
      }

      vmService = await vmServiceConnectUri(
        'ws://localhost:${info.serverUri!.port}${info.serverUri!.path}ws',
      );

      dynamic error;
      try {
        await vmService.callMethod(
          '_flutter.setAssetBundlePath',
          args: <String, Object>{'assetDirectory': ''},
        );
      } catch (err) {
        error = err;
      }
      expect(error != null, true);
    } finally {
      await vmService?.dispose();
    }
  });
}

Future<String?> getIsolateId(vms.VmService vmService) async {
  final vms.VM vm = await vmService.getVM();
  for (final vms.IsolateRef isolate in vm.isolates!) {
    if (isolate.isSystemIsolate ?? false) {
      continue;
    }
    return isolate.id;
  }
  return null;
}
