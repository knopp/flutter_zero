// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:convert';
import 'dart:io' as io;

import 'package:path/path.dart' as pathlib;
// TODO(yjbanov): remove hacks when this is fixed:
//                https://github.com/dart-lang/test/issues/1521
import 'package:test_api/backend.dart' as hack;
import 'package:test_core/src/executable.dart' as test;
import 'package:test_core/src/runner/hack_register_platform.dart' as hack;

import '../browser.dart';
import '../common.dart';
import '../environment.dart';
import '../exceptions.dart';
import '../felt_config.dart';
import '../pipeline.dart';
import '../test_platform.dart';
import '../utils.dart';

/// Runs a test suite.
///
/// Assumes the artifacts from previous steps are available, either from
/// running them prior to this step locally, or by having the build graph copy
/// them from another bot.
class RunSuiteStep implements PipelineStep {
  RunSuiteStep(
    this.suite, {
    required this.startPaused,
    required this.isVerbose,
    required this.overridePathToCanvasKit,
    required this.useDwarf,
    this.testFiles,
  });

  final TestSuite suite;
  final Set<FilePath>? testFiles;
  final bool startPaused;
  final bool isVerbose;
  final String? overridePathToCanvasKit;
  final bool useDwarf;

  @override
  String get description => 'run_suite';

  @override
  bool get isSafeToInterrupt => true;

  @override
  Future<void> interrupt() async {}

  @override
  Future<void> run() async {
    _prepareTestResultsDirectory();
    final BrowserEnvironment browserEnvironment = getBrowserEnvironment(
      suite.runConfig.browser,
      useDwarf: useDwarf,
      browserFlags: suite.runConfig.browserFlags,
    );
    await browserEnvironment.prepare();

    final String configurationFilePath = pathlib.join(
      environment.webUiRootDir.path,
      browserEnvironment.packageTestConfigurationYamlFile,
    );
    final String bundleBuildPath = getBundleBuildDirectory(suite.testBundle).path;
    final testArgs = <String>[
      ...<String>['-r', 'compact'],
      // Disable concurrency. Running with concurrency proved to be flaky.
      '--concurrency=1',
      if (startPaused) '--pause-after-load',
      '--platform=${browserEnvironment.packageTestRuntime.identifier}',
      '--precompiled=$bundleBuildPath',
      '--configuration=$configurationFilePath',
      if (AnsiColors.shouldEscape) '--color' else '--no-color',

      // TODO(jacksongardner): Set the default timeout to five minutes when
      // https://github.com/dart-lang/test/issues/2006 is fixed.
      '--',
      ..._collectTestPaths(),
    ];

    hack.registerPlatformPlugin(<hack.Runtime>[browserEnvironment.packageTestRuntime], () {
      return BrowserPlatform.start(
        suite,
        browserEnvironment: browserEnvironment,
        overridePathToCanvasKit: overridePathToCanvasKit,
        isVerbose: isVerbose,
      );
    });

    print('[${suite.name.ansiCyan}] Running...');

    // We want to run tests with the test set's directory as a working directory.
    final testSetDirectory = io.Directory(
      pathlib.join(environment.webUiTestDir.path, suite.testBundle.testSet.directory),
    );
    final dynamic originalCwd = io.Directory.current;
    io.Directory.current = testSetDirectory;
    try {
      await test.main(testArgs);
    } finally {
      io.Directory.current = originalCwd;
    }

    await browserEnvironment.cleanup();

    // Since we are just calling `main()` on the test executable, it will modify
    // the exit code. We use this as a signal that there were some tests that failed.
    if (io.exitCode != 0) {
      print('[${suite.name.ansiCyan}] ${'Some tests failed.'.ansiRed}');
      // Change the exit code back to 0 when we're done. Failures will be bubbled up
      // at the end of the pipeline and we'll exit abnormally if there were any
      // failures in the pipeline.
      io.exitCode = 0;
      throw ToolExit('Some unit tests failed in suite ${suite.name.ansiCyan}.');
    } else {
      print('[${suite.name.ansiCyan}] ${'All tests passed!'.ansiGreen}');
    }
  }

  io.Directory _prepareTestResultsDirectory() {
    final resultsDirectory = io.Directory(
      pathlib.join(environment.webUiTestResultsDirectory.path, suite.name),
    );
    if (resultsDirectory.existsSync()) {
      resultsDirectory.deleteSync(recursive: true);
    }
    resultsDirectory.createSync(recursive: true);
    return resultsDirectory;
  }

  List<String> _collectTestPaths() {
    final io.Directory bundleBuild = getBundleBuildDirectory(suite.testBundle);
    final resultsJsonFile = io.File(pathlib.join(bundleBuild.path, 'results.json'));
    if (!resultsJsonFile.existsSync()) {
      throw ToolExit(
        'Could not find built bundle ${suite.testBundle.name.ansiMagenta} for suite ${suite.name.ansiCyan}.',
      );
    }
    final String jsonString = resultsJsonFile.readAsStringSync();
    final jsonContents = const JsonDecoder().convert(jsonString) as Map<String, Object?>;
    final results = jsonContents['results']! as Map<String, Object?>;
    final testPaths = <String>[];
    results.forEach((Object? k, Object? v) {
      final result = v! as String;
      final testPath = k! as String;
      if (testFiles != null) {
        if (!testFiles!.contains(FilePath.fromTestSet(suite.testBundle.testSet, testPath))) {
          return;
        }
      }
      if (result == 'success') {
        testPaths.add(testPath);
      }
    });
    return testPaths;
  }
}
