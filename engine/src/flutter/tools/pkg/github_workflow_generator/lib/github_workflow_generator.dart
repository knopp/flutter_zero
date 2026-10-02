// ignore_for_file: avoid_print, specify_nonobvious_local_variable_types, public_member_api_docs

import 'dart:convert';
import 'dart:io';

import 'package:args/args.dart';
import 'package:engine_build_configs/engine_build_configs.dart';
import 'package:path/path.dart' as path;

class YamlWriter {
  YamlWriter() {
    root = YamlWriterSection._(this, isArray: false);
  }

  late final YamlWriterSection root;

  final _buffer = StringBuffer();
}

class _Artifact {
  _Artifact({
    required this.artifactName,
    required this.sourcePath,
    required this.outputPath,
    required this.id,
  });

  final String artifactName;
  final String sourcePath;
  final String outputPath;
  final int id;
}

/// Gathers all artifacts and and publishes them as a very last job
/// in the workflow (to prevent uploading artifacts when some of the workflows fail).
class ArtifactPublisher {
  void uploadArtifact(
    YamlWriterSection steps, {
    required String sourceJobName,
    required String sourcePath,
    required String outputPath,
  }) {
    // First tar the artifact to preserve attributes.
    final id = _count++;
    final sourceDir = path.dirname(sourcePath);
    final sourceName = path.basename(sourcePath);
    final name = 'artifact_\${{ steps.engine_content_hash.outputs.value }}_$id';
    {
      final step = steps.beginMap('name', 'Tar $sourcePath');
      final run = step.beginMap('run', '|');
      run.writeln('cd $sourceDir');
      run.writeln('tar -cvf artifact_$id.tar $sourceName');
    }
    {
      final step = steps.beginMap('name', 'Upload $outputPath/$sourceJobName');
      step.write('uses', 'actions/upload-artifact@v4');
      final w = step.beginMap('with');
      w.write('name', name);
      w.write('path', '$sourceDir/artifact_$id.tar');
      w.write('retention-days', '1');
    }
    _dependentJobs.add(sourceJobName);
    _artifacts.add(
      _Artifact(
        artifactName: name,
        sourcePath: sourcePath,
        outputPath: outputPath,
        id: id,
      ),
    );
  }

  void registerTestJob(String jobName) {
    _dependentTestJobs.add(jobName);
  }

  /// Publishes everything this run produced. On staging pushes the artifacts
  /// go to the production bucket, and the `latest_content_hash` pointer is
  /// advanced. On pull requests everything goes under a unique per-run prefix
  /// of the CI-only bucket instead - a disposable deployment that the
  /// `remove_ci_artifacts` teardown job of the same run deletes afterwards.
  void writePublishJob(YamlWriterSection jobsSection) {
    if (_artifacts.isEmpty) {
      return;
    }
    final job = jobsSection.beginMap('publish_artifacts');
    final defaults = job.beginMap('defaults');
    final run = defaults.beginMap('run');
    run.writeln('shell: bash');
    final needs = job.beginArray('needs');
    _dependentJobs.forEach(needs.writeln);
    _dependentTestJobs.forEach(needs.writeln);
    needs.writeln('guard');
    job.write('runs-on', 'ubuntu-latest');
    job.write(
      'if',
      r"${{ needs.guard.outputs.should_run == 'true' }}",
    );
    final steps = job.beginArray('steps');
    {
      final step = steps.beginMap('name', 'Expose engine content hash');
      step.write('id', 'engine_content_hash');
      final run = step.beginMap('run', '|');
      run.writeln(r'engine_content_hash=${{ needs.guard.outputs.engine_content_hash }}');
      run.writeln(r'echo "value=${engine_content_hash}" >> $GITHUB_OUTPUT');
    }
    for (final artifact in _artifacts) {
      final name = path.basename(artifact.sourcePath);
      {
        final step = steps.beginMap('name', 'Download ${artifact.outputPath}/$name');
        step.write('uses', 'actions/download-artifact@v4');
        final w = step.beginMap('with');
        w.write('name', artifact.artifactName);
        w.write('path', 'artifact-${artifact.id}/');
      }
      {
        // Extract the tarball.
        final step = steps.beginMap('name', 'Extract ${artifact.outputPath}/$name');
        final run = step.beginMap('run', '|');
        run.writeln('tar -xvf artifact-${artifact.id}/artifact_${artifact.id}.tar -C artifact-${artifact.id}/');
        run.writeln('rm artifact-${artifact.id}/artifact_${artifact.id}.tar');
      }
    }
    {
      // Selects the R2 destination for this run once for all publish
      // steps: pull requests deploy under a unique per-run prefix of the
      // CI-only bucket (disposable, the run's teardown job removes it);
      // staging pushes deploy to the production bucket with no prefix and
      // advance latest_content_hash. The same credential secrets have write
      // access to both buckets.
      final step = steps.beginMap('name', 'Select the R2 target');
      step.write('id', 'r2_target');
      final runEnv = step.beginMap('env');
      runEnv.writeln('R2_ENGINE_BUCKET: flutter-zero-engine');
      runEnv.writeln('R2_CI_BUCKET: $ciBucketName');
      final run = step.beginMap('run', '|');
      run.writeln(r'if [ "${GITHUB_EVENT_NAME}" = "pull_request" ]; then');
      run.writeln(r'  echo "bucket=$R2_CI_BUCKET" >> $GITHUB_OUTPUT');
      run.writeln(r'  echo "prefix=pr-${GITHUB_RUN_ID}/" >> $GITHUB_OUTPUT');
      run.writeln(r'else');
      run.writeln(r'  echo "bucket=$R2_ENGINE_BUCKET" >> $GITHUB_OUTPUT');
      run.writeln(r'  echo "prefix=" >> $GITHUB_OUTPUT');
      run.writeln(r'fi');
    }
    for (final artifact in _artifacts) {
      final name = path.basename(artifact.sourcePath);
      // Pull requests deploy under the selected per-run prefix; staging
      // pushes deploy (as before) keyed directly by the engine content hash
      // with no prefix.
      final baseDestination = name.contains('flutter.io')
          // android
          ? ''
          : 'flutter_infra_release/flutter/\${{ steps.engine_content_hash.outputs.value }}/${artifact.outputPath}';
      _writeR2UploadStep(
        steps,
        name: 'Publish ${artifact.outputPath}/$name',
        bucket: '\${{ steps.r2_target.outputs.bucket }}',
        destinationDir: '\${{ steps.r2_target.outputs.prefix }}$baseDestination',
        sourceDir: 'artifact-${artifact.id}/',
      );
    }
    {
      final step = steps.beginMap('name', 'Create latest_content_hash.txt');
      step.write('if', r"${{ github.event_name != 'pull_request' }}");
      final run = step.beginMap('run', '|');
      run.writeln(r'mkdir -p latest_content_hash');
      run.writeln(
        r'echo "${{ needs.guard.outputs.engine_content_hash }}" > latest_content_hash/latest_content_hash.txt',
      );
    }
    {
      _writeR2UploadStep(
        steps,
        name: 'Publish latest_content_hash.txt',
        ifCondition: r"${{ github.event_name != 'pull_request' }}",
        bucket: '\${{ steps.r2_target.outputs.bucket }}',
        destinationDir: 'flutter_infra_release/flutter/',
        sourceDir: 'latest_content_hash/',
        id: 'publish_latest_content_hash',
      );
    }
  }

  void _writeR2UploadStep(
    YamlWriterSection steps, {
    required String name,
    required String bucket,
    required String destinationDir,
    required String sourceDir,
    String? ifCondition,
    String? id,
  }) {
    final step = steps.beginMap('name', name);
    if (ifCondition != null) {
      step.write('if', ifCondition);
    }
    if (id != null) {
      step.write('id', id);
    }
    step.write('uses', r2UploadAction);
    final w = step.beginMap('with');
    w.write('r2-account-id', '\${{ secrets.R2_ACCOUNT_ID }}');
    w.write('r2-access-key-id', '\${{ secrets.R2_ACCESS_KEY_ID }}');
    w.write('r2-secret-access-key', '\${{ secrets.R2_SECRET_ACCESS_KEY }}');
    w.write('r2-bucket', bucket);
    w.write('source-dir', sourceDir);
    w.write('destination-dir', destinationDir.isEmpty ? './' : destinationDir);
  }

  static const r2UploadAction = 'ryand56/r2-upload-action@b801a390acbdeb034c5e684ff5e1361c06639e7c';

  final _dependentTestJobs = <String>{};
  final _dependentJobs = <String>{};
  final _artifacts = <_Artifact>[];
  int _count = 0;
}

class YamlWriterSection {
  YamlWriterSection._(this.writer, {required bool isArray}) : _isArray = isArray;

  final YamlWriter writer;

  void writeln(String line) {
    final arrayPrefix = _isArray ? '- ' : '';
    writer._buffer.writeln('${'  ' * _indentationLevel}$arrayPrefix$line');
  }

  void write(String label, String value) {
    final line = value.isEmpty ? '$label:' : '$label: ${_escapeScalar(value)}';
    writeln(line);
  }

  static String _escapeScalar(String value) {
    // '|' is written bare for run-step block scalars (beginMap('run', '|')).
    if (value == '|') {
      return value;
    }
    // A plain (unquoted) scalar must not look like a comment, a nested map, or
    // contain characters that terminate the scalar early (':' followed by
    // whitespace, or a ' #' comment start).
    final problematic =
        value.startsWith(' ') ||
        value.endsWith(' ') ||
        value.contains(': ') ||
        value.endsWith(':') ||
        value.contains(' #');
    if (!problematic) {
      return value;
    }
    final escaped = value.replaceAll("'", "''");
    return "'$escaped'";
  }

  YamlWriterSection beginMap(String label, [String value = '']) {
    write(label, value);
    final section = YamlWriterSection._(writer, isArray: false);
    section._indentationLevel = _indentationLevel + 1;
    return section;
  }

  YamlWriterSection beginArray(String label, [String value = '']) {
    write(label, value);
    final section = YamlWriterSection._(writer, isArray: true);
    section._indentationLevel = _indentationLevel + 1;
    return section;
  }

  int _indentationLevel = 0;
  final bool _isArray;
}

class BuildConfigWriter {
  BuildConfigWriter({
    required BuilderConfig config,
    required YamlWriterSection jobsSections,
    required ArtifactPublisher artifactPublisher,
    required Set<String> testBuildOutputs,
  }) : _config = config,
       _jobsSections = jobsSections,
       _artifactPublisher = artifactPublisher,
       _testBuildOutputs = testBuildOutputs;

  void write() {
    for (final build in _config.builds) {
      final job = _jobsSections.beginMap(_nameForBuild(build));
      job.write('runs-on', _getRunnerForBuilder(build));
      final defaults = job.beginMap('defaults');
      final run = defaults.beginMap('run');
      run.writeln('shell: bash');
      final needs = job.beginArray('needs');
      needs.writeln('guard');
      job.write('if', r"${{ needs.guard.outputs.should_run == 'true' }}");
      final steps = job.beginArray('steps');

      _writePrelude(steps);
      {
        final step = steps.beginMap('name', 'Build engine');
        if (_getRunnerForBuilder(build).startsWith('windows')) {
          // et.sh refuses to run on Windows, so we need to use et.bat
          step.write('shell', 'cmd');
          final run = step.beginMap('run', '|');
          run.writeln(r'cd engine\src');
          run.writeln('flutter\\bin\\et.bat build --config ${build.name}');
        } else {
          final run = step.beginMap('run', '|');
          run.writeln('cd engine/src');
          run.writeln('./flutter/bin/et build --config ${build.name}');
        }
      }
      // Host builds that compile the unit test binaries run them in-place -
      // no artifact download needed.
      if (build.ninja.targets.contains('flutter:unittests')) {
        final bool isWindows = _getRunnerForBuilder(build).startsWith('windows');
        final step = steps.beginMap('name', 'Run engine unit tests');
        final run = step.beginMap('run', '|');
        run.writeln('cd engine/src');
        // Windows runners only provide an unprefixed `python` executable.
        run.writeln('${isWindows ? 'python' : 'python3'} flutter/testing/run_tests.py \\');
        // Forward slashes: the step runs under bash even on Windows
        // runners, where a backslash would escape the next character.
        run.writeln('  --variant ${build.name.replaceAll(r'\', '/')} \\');
        run.writeln('  --type engine \\');
        if (!isWindows) {
          run.writeln('  --engine-capture-core-dump');
        }
      }
      for (final generator in build.generators) {
        _writeBuildTask(steps, generator);
      }
      // Full build outputs are needed only by global packaging or test jobs.
      if (_config.generators.isNotEmpty || _config.archives.isNotEmpty ||
          _testBuildOutputs.contains(_nameForBuild(build))) {
      {
        final step = steps.beginMap('name', 'Tar build files');
        final run = step.beginMap('run', '|');
        // The out dir is derived from the ninja config (which may lack the
        // "ci/" prefix, e.g. the web wasm_release build), not from the build
        // name.
        final outDir = build.ninja.config.isEmpty
            ? build.name.replaceAll(r'\', '/')
            : build.ninja.config.replaceAll(r'\', '/');
        run.writeln('find engine/src/out/$outDir -type l -lname "/*" -delete || true');
        run.writeln('tar -cvf ${_nameForBuild(build)}.tar engine/src/out/$outDir');
      }
      {
        final step = steps.beginMap('name', 'Upload build files');
        step.write('uses', 'actions/upload-artifact@v4');
        final w = step.beginMap('with');
        w.write('name', 'artifacts-${_nameForBuild(build)}-\${{ steps.engine_content_hash.outputs.value }}');
        w.write('path', '${_nameForBuild(build)}.tar');
        w.write('retention-days', '1');
      }
      }
      for (final archive in build.archives) {
        for (final assetPath in archive.includePaths) {
          if (!assetPath.startsWith(archive.basePath)) {
            throw Exception('Archive include path $assetPath does not start with base path ${archive.basePath}');
          }
          final relativePath = assetPath.substring(archive.basePath.length);
          var relativePathDir = path.dirname(relativePath);
          if (relativePathDir == '.') {
            relativePathDir = '';
          }
          _artifactPublisher.uploadArtifact(
            steps,
            sourceJobName: _nameForBuild(build),
            sourcePath: 'engine/src/$assetPath',
            outputPath: relativePathDir,
          );
        }
      }
    }
    if (_config.generators.isNotEmpty || _config.archives.isNotEmpty) {
      final globalJobName = '${path.basenameWithoutExtension(_config.path)}_global';
      final job = _jobsSections.beginMap(globalJobName);
      job.write('runs-on', _getRunnerForBuilder(_config.builds.first));
      final needs = job.beginArray('needs');
      for (final build in _config.builds) {
        needs.writeln(_nameForBuild(build));
      }
      needs.writeln('guard');
      final defaults = job.beginMap('defaults');
      final run = defaults.beginMap('run');
      run.writeln('shell: bash');
      job.write('if', r"${{ needs.guard.outputs.should_run == 'true' }}");
      final steps = job.beginArray('steps');
      _writePrelude(steps);
      for (final build in _config.builds) {
        final step = steps.beginMap('name', 'Download Artifacts from ${_nameForBuild(build)}');
        {
          step.write('uses', 'actions/download-artifact@v4');
          final w = step.beginMap('with');
          w.write('name', 'artifacts-${_nameForBuild(build)}-\${{ steps.engine_content_hash.outputs.value }}');
        }
        {
          final step = steps.beginMap('name', 'Extract Artifacts from ${_nameForBuild(build)}');
          final run = step.beginMap('run', '|');
          run.writeln('tar -xvf ${_nameForBuild(build)}.tar');
          run.writeln('rm ${_nameForBuild(build)}.tar');
        }
      }
      for (final generator in _config.generators) {
        _writeTestTask(steps, generator);
      }
      for (final archive in _config.archives) {
        if (path.basename(archive.source) != path.basename(archive.destination)) {
          throw Exception(
            'Global archive source and destination must have the same filename: ${archive.source} vs ${archive.destination}',
          );
        }
        var relativePathDir = path.dirname(archive.destination);
        if (relativePathDir == '.') {
          relativePathDir = '';
        }
        _artifactPublisher.uploadArtifact(
          steps,
          sourceJobName: globalJobName,
          sourcePath: 'engine/src/${archive.source}',
          outputPath: relativePathDir,
        );
      }
    }
  }

  void _writePrelude(YamlWriterSection steps) {
    {
      final step = steps.beginMap('name', 'Checkout the repository');
      step.write('uses', 'actions/checkout@v4');
      final w = step.beginMap('with');
      w.write('path', "''");
    }
    {
      _writeDepotToolsStep(steps, ifCondition: "runner.os != 'Windows'", isWindows: false);
    }
    if (_config.generators.any((b) => b.name == 'api-documentation')) {
      final step = steps.beginMap('name', 'Install doxygen');
      step.write('if', "runner.os == 'Linux'");
      step.write('uses', 'ssciwr/doxygen-install@501e53b879da7648ab392ee226f5b90e42148449');
      final w = step.beginMap('with');
      w.write('version', '1.14.0');
    }
    {
      final step = steps.beginMap('name', 'Free disk space');
      // The script fails on arm64 Linux runners.
      step.write('if', "runner.os == 'Linux' && runner.arch == 'X64'");
      step.write(
        'run',
        'curl -fsSL https://raw.githubusercontent.com/apache/arrow/e49d8ae15583ceff03237571569099a6ad62be32/ci/scripts/util_free_space.sh | bash',
      );
    }
    {
      _writeDepotToolsStep(steps, ifCondition: "runner.os == 'Windows'", isWindows: true);
    }
    {
      final step = steps.beginMap('name', 'Generate engine content hash');
      step.write('id', 'engine_content_hash');
      final run = step.beginMap('run', '|');
      run.writeln(r'engine_content_hash=${{ needs.guard.outputs.engine_content_hash }}');
      run.writeln(r'echo "value=${engine_content_hash}" >> $GITHUB_OUTPUT');
    }
    {
      final step = steps.beginMap('name', 'Copy gclient file');
      final run = step.beginMap('run', '|');
      // Android toolchains are only fetched when the standard gclient file
      // is used. This must cover configs that build android targets even
      // when the config file itself is not named after android (e.g.
      // linux_unopt.json).
      final needsAndroidDeps =
          path.basename(_config.path).contains('android') ||
          _config.builds.any((build) => build.gn.contains('--android'));
      if (needsAndroidDeps) {
        run.writeln('cp engine/scripts/standard.gclient .gclient');
      } else if (path.basename(_config.path).contains('web')) {
        run.writeln('cp engine/scripts/web.gclient .gclient');
      } else {
        run.writeln('cp engine/scripts/slim.gclient .gclient');
      }
    }
    {
      final step = steps.beginMap('name', 'GClient sync');
      final run = step.beginMap('run', '|');
      run.writeln('gclient sync -D --no-history --shallow --with_branch_heads');
    }
  }

  String _taskLauncherScript(String script, {required String language}) {
    if (language == 'dart') {
      // Use existing prebuilt engine version for the Dart SDK, as this is
      // executed before publishing new engine artifacts.
      return 'FLUTTER_PREBUILT_ENGINE_VERSION=\${{ needs.guard.outputs.latest_content_hash }} ../../bin/dart $script';
    } else if (language == 'python3') {
      return 'python3 $script';
    } else if (language == 'python') {
      return 'python $script';
    } else if (language == 'bash' || language == '<undef>') {
      return script;
    } else {
      throw Exception('Unsupported generator language: $language');
    }
  }

  void _writeBuildTask(YamlWriterSection steps, BuildTask generator) {
    final step = steps.beginMap('name', 'Run generator ${generator.name}');
    final run = step.beginMap('run', '|');
    run.writeln('cd engine/src');
    for (final script in generator.scripts) {
      final launcher = _taskLauncherScript(script, language: generator.language);
      if (generator.parameters.isEmpty) {
        run.writeln(launcher);
      } else {
        run.writeln('$launcher \\');
        for (final (index, arg) in generator.parameters.indexed) {
          final suffix = index == generator.parameters.length - 1 ? '' : r' \';
          run.writeln('  $arg$suffix');
        }
      }
    }
  }

  void _writeTestTask(YamlWriterSection steps, TestTask generator) {
    final step = steps.beginMap('name', 'Run generator ${generator.name}');
    final run = step.beginMap('run', '|');
    run.writeln('cd engine/src');
    final launcher = _taskLauncherScript(generator.script, language: generator.language);
    if (generator.parameters.isEmpty) {
      run.writeln(launcher);
    } else {
      run.writeln('$launcher \\');
      for (final (index, arg) in generator.parameters.indexed) {
        final suffix = index == generator.parameters.length - 1 ? '' : r' \';
        run.writeln('  $arg$suffix');
      }
    }
  }

  String _getRunnerForBuilder(Build build) {
    final bool isArm = path.basename(_config.path).contains('_arm_');
    for (final record in build.droneDimensions) {
      if (record.startsWith('os=Mac')) {
        return 'macos-latest';
      } else if (record.startsWith('os=Linux')) {
        return isArm ? 'ubuntu-24.04-arm' : 'ubuntu-latest';
      } else if (record.startsWith('os=Windows')) {
        return isArm ? 'windows-11-arm' : 'windows-2022';
      }
    }
    throw Exception('Unknown OS for build: ${build.name}');
  }

  String _nameForBuild(Build build) {
    var name = build.name.replaceAll(r'\', '/');
    if (!name.startsWith('ci/')) {
      throw Exception('Unexpected build name format: $name');
    }
    name = name.substring(3); // Remove 'ci/' prefix.
    final prefix = _prefix();
    if (name.startsWith('${prefix}_')) {
      return name;
    } else {
      return '${_prefix()}_$name';
    }
  }

  String _prefix() {
    // Get the prefix from config path filename (first part before underscore).
    final filename = path.basename(_config.path);
    final prefix = filename.split('_').first;
    return prefix;
  }

  final BuilderConfig _config;
  final YamlWriterSection _jobsSections;
  final ArtifactPublisher _artifactPublisher;
  final Set<String> _testBuildOutputs;
}

/// A single test run inside an artifact test job.
class _ArtifactTest {
  const _ArtifactTest({
    required this.name,
    required this.variant,
    required this.types,
    this.androidVariant,
    this.iosVariant,
  });

  final String name;
  final String variant;
  final String types;

  final String? androidVariant;
  final String? iosVariant;
}

/// A generated test job that runs against build outputs uploaded by regular
/// build jobs, without rebuilding anything.
class _ArtifactTestJob {
  const _ArtifactTestJob({
    required this.name,
    required this.runsOn,
    required this.python,
    required this.gclient,
    required this.downloads,
    required this.tests,
  });

  final String name;
  final String runsOn;

  /// Python executable used to invoke run_tests.py (GitHub Windows runners
  /// only provide an unprefixed `python`).
  final String python;

  /// Which gclient file to sync with (slim, standard or web).
  final String gclient;

  /// Names of build jobs whose full build-output tar is downloaded and
  /// extracted before the tests run.
  final List<String> downloads;
  final List<_ArtifactTest> tests;

  static _ArtifactTest _testFromJson(Map<String, Object?> map) {
    final variant = map['variant']! as String;
    final types = map['types']! as String;
    final androidVariant = map['android_variant'] as String?;
    final iosVariant = map['ios_variant'] as String?;
    return _ArtifactTest(
      name: (map['name'] as String?) ?? 'for $variant',
      variant: variant,
      types: types,
      androidVariant: androidVariant,
      iosVariant: iosVariant,
    );
  }

  static _ArtifactTestJob fromJson(Map<String, Object?> map) {
    final name = map['name']! as String;
    final runsOn = map['runs_on']! as String;
    final python = map['python'] as String? ?? 'python3';
    final gclient = map['gclient'] as String? ?? 'slim';
    final downloads = (map['downloads'] as List<Object?>? ?? []).cast<String>();
    final tests = (map['tests'] as List<Object?>? ?? []).cast<Map<String, Object?>>().map(_testFromJson).toList();
    if (downloads.isEmpty) {
      throw Exception('Artifact test job $name has no downloads');
    }
    if (tests.isEmpty) {
      throw Exception('Artifact test job $name has no tests');
    }
    return _ArtifactTestJob(
      name: name,
      runsOn: runsOn,
      python: python,
      gclient: gclient,
      downloads: downloads,
      tests: tests,
    );
  }
}

/// Emits a depot_tools setup step: cleans up any checkout left corrupt by a
/// failed previous attempt (the same HOME is reused across job retries), then
/// clones with retries - clones from chromium.googlesource.com occasionally
/// fail mid-transfer and the bare command is the most common source of CI
/// flake.
void _writeDepotToolsStep(
  YamlWriterSection steps, {
  String? ifCondition,
  required bool isWindows,
}) {
  final step = steps.beginMap('name', 'Set up depot_tools');
  if (ifCondition != null) {
    step.write('if', ifCondition);
  }
  final run = step.beginMap('run', '|');
  run.writeln(r'rm -rf "$HOME/depot_tools"');
  run.writeln('for attempt in 1 2 3; do');
  run.writeln(
    r'  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$HOME/depot_tools" && break',
  );
  run.writeln(r'  rm -rf "$HOME/depot_tools"');
  run.writeln(r'  echo "depot_tools clone failed (attempt $attempt), retrying..."');
  run.writeln('  sleep 5');
  run.writeln('done');
  run.writeln(r'test -d "$HOME/depot_tools/.git"');
  run.writeln('# Append depot_tools to the PATH for subsequent steps');
  if (isWindows) {
    run.writeln(r'DEPOT_TOOLS_WIN=$(cygpath -w "$HOME/depot_tools")');
    run.writeln(r'echo "$DEPOT_TOOLS_WIN" >> $GITHUB_PATH');
  } else {
    run.writeln(r'echo "$HOME/depot_tools" >> $GITHUB_PATH');
  }
}

/// Emits jobs that run tests against build outputs uploaded by the
/// artifact-producing build jobs (no rebuilding). The job specs come from the
/// JSON files passed via `--artifact-tests`.
List<_ArtifactTestJob> _readArtifactTestJobs(List<String> specFiles) {
  final jobs = <_ArtifactTestJob>[];
  for (final specFile in specFiles) {
    final content = File(specFile).readAsStringSync();
    final map = jsonDecode(content) as Map<String, Object?>;
    final jobMaps = (map['test_jobs'] as List<Object?>? ?? []).cast<Map<String, Object?>>();
    if (jobMaps.isEmpty) {
      throw Exception('Artifact test spec $specFile has no test_jobs');
    }
    jobs.addAll(jobMaps.map(_ArtifactTestJob.fromJson));
  }
  return jobs;
}

void _writeArtifactTestJobs(
  YamlWriterSection jobsSection,
  ArtifactPublisher artifactPublisher,
  List<_ArtifactTestJob> jobs,
) {
  const gclientFileFor = {
    'slim': 'slim.gclient',
    'standard': 'standard.gclient',
    'web': 'web.gclient',
  };
  for (final job in jobs) {
    final generated = jobsSection.beginMap(job.name);
    generated.write('runs-on', job.runsOn);
    final defaults = generated.beginMap('defaults');
    final runDefaults = defaults.beginMap('run');
    runDefaults.writeln('shell: bash');
    final needs = generated.beginArray('needs');
    job.downloads.forEach(needs.writeln);
    needs.writeln('guard');
    generated.write('if', r"${{ needs.guard.outputs.should_run == 'true' }}");
    final steps = generated.beginArray('steps');
    {
      final step = steps.beginMap('name', 'Checkout the repository');
      step.write('uses', 'actions/checkout@v4');
      final w = step.beginMap('with');
      w.write('path', "''");
    }
    {
      _writeDepotToolsStep(steps, isWindows: job.runsOn.startsWith('windows'));
    }
    {
      final step = steps.beginMap('name', 'Expose engine content hash');
      step.write('id', 'engine_content_hash');
      final run = step.beginMap('run', '|');
      run.writeln(r'engine_content_hash=${{ needs.guard.outputs.engine_content_hash }}');
      run.writeln(r'echo "value=${engine_content_hash}" >> $GITHUB_OUTPUT');
    }
    {
      final step = steps.beginMap('name', 'Copy gclient file');
      final run = step.beginMap('run', '|');
      final gclientFile = gclientFileFor[job.gclient];
      if (gclientFile == null) {
        throw Exception('Unknown gclient flavor ${job.gclient} in artifact test job ${job.name}');
      }
      run.writeln('cp engine/scripts/$gclientFile .gclient');
    }
    {
      final step = steps.beginMap('name', 'GClient sync');
      final run = step.beginMap('run', '|');
      run.writeln('gclient sync -D --no-history --shallow --with_branch_heads');
    }
    for (final download in job.downloads) {
      {
        final step = steps.beginMap('name', 'Download Artifacts from $download');
        step.write('uses', 'actions/download-artifact@v4');
        final w = step.beginMap('with');
        w.write('name', 'artifacts-$download-\${{ steps.engine_content_hash.outputs.value }}');
      }
      {
        final step = steps.beginMap('name', 'Extract Artifacts from $download');
        final run = step.beginMap('run', '|');
        run.writeln('tar -xvf $download.tar');
        run.writeln('rm $download.tar');
      }
    }
    for (final test in job.tests) {
      final step = steps.beginMap('name', 'Run test ${test.name}');
      final run = step.beginMap('run', '|');
      run.writeln('cd engine/src');
      run.writeln('${job.python} flutter/testing/run_tests.py \\');
      run.writeln('  --variant ${test.variant} \\');
      run.writeln('  --type ${test.types} \\');
      if (test.androidVariant != null) {
        run.writeln('  --android-variant ${test.androidVariant} \\');
      }
      if (test.iosVariant != null) {
        run.writeln('  --ios-variant ${test.iosVariant} \\');
      }
      // --engine-capture-core-dump is not supported on Windows runners.
      if (!job.runsOn.startsWith('windows')) {
        run.writeln('  --engine-capture-core-dump');
      }
    }
    artifactPublisher.registerTestJob(job.name);
  }
}

/// Emits the job that invokes the smoke test workflow with the per-run prefix
/// of this deployment (the same run). Only for pull requests; on staging
/// pushes the smoke test flow is exercised by the call-smoke-test companion
/// workflow instead. The smoke test workflow itself is call-only.
void _writeSmokeTestInvocationJob(YamlWriterSection jobsSection) {
  final job = jobsSection.beginMap('smoke_test');
  job.write('name', 'Run the smoke test');
  job.write('uses', './.github/workflows/smoke-test.yml');
  final needs = job.beginArray('needs');
  needs.writeln('publish_artifacts');
  job.write(
    'if',
    r"${{ github.event_name == 'pull_request' }}",
  );
  final withNode = job.beginMap('with');
  withNode.write('run_id', r'${{ github.run_id }}');
}

/// Emits the teardown job of the CI-only artifacts bucket: the per-run prefix
/// this run published is removed again, unconditionally (always(), pass,
/// fail, cancel - the run's deployment is disposable).
void _writeCiBucketTeardownJob(
  YamlWriterSection jobsSection,
  Set<String> targets,
) {
  final job = jobsSection.beginMap('remove_ci_artifacts');
  job.write('runs-on', 'ubuntu-latest');
  final needs = job.beginArray('needs');
  targets.forEach(needs.writeln);
  job.write('if', 'always()');
  final steps = job.beginArray('steps');
  {
    final step = steps.beginMap('name', 'Remove the run prefix from the CI bucket');
    step.write('if', r"${{ github.event_name == 'pull_request' }}");
    final runEnv = step.beginMap('env');
    runEnv.writeln(
      'AWS_ACCESS_KEY_ID: \${{ secrets.R2_ACCESS_KEY_ID }}',
    );
    runEnv.writeln(
      'AWS_SECRET_ACCESS_KEY: \${{ secrets.R2_SECRET_ACCESS_KEY }}',
    );
    runEnv.writeln(
      'AWS_ENDPOINT_URL_S3: $r2S3Endpoint',
    );
    runEnv.writeln('AWS_DEFAULT_REGION: auto');
    final run = step.beginMap('run', '|');
    run.writeln(
      'aws s3 rm s3://$ciBucketName/pr-\${{ github.run_id }}/ --recursive',
    );
  }
}

/// The CI-only artifacts bucket: pull request deployments are published under
/// a unique per-run prefix here, giving short-lived, disposable copies that
/// never touch the production buckets.
const ciBucketName = 'flutter-zero-ci';
const r2S3Endpoint = 'https://b468378afe5657ffecccb682fe82ccac.r2.cloudflarestorage.com';

void _writeGuardJob(YamlWriterSection jobsSection) {
  final job = jobsSection.beginMap('guard');
  job.write('runs-on', 'ubuntu-latest');
  final outputs = job.beginMap('outputs');
  outputs.write('should_run', r'${{ steps.check.outputs.should_run }}');
  outputs.write('engine_content_hash', r'${{ steps.engine_content_hash.outputs.value }}');
  outputs.write('latest_content_hash', r'${{ steps.fetch_latest_content_hash.outputs.value }}');
  final steps = job.beginArray('steps');
  {
    final step = steps.beginMap('name', 'Checkout the repository');
    step.write('uses', 'actions/checkout@v4');
    final w = step.beginMap('with');
    w.write('path', "''");
  }
  {
    final step = steps.beginMap('name', 'Generate engine content hash');
    step.write('id', 'engine_content_hash');
    final run = step.beginMap('run', '|');
    run.writeln(r'engine_content_hash=$(bin/internal/content_aware_hash.sh)');
    run.writeln(r'echo "::notice:: Engine content hash: ${engine_content_hash}"');
    run.writeln(r'echo "value=${engine_content_hash}" >> $GITHUB_OUTPUT');
  }
  {
    final step = steps.beginMap('name', 'Check if engine.stamp exists');
    step.write('id', 'check');
    final run = step.beginMap('run', '|');
    run.writeln(
      r'URL="https://engine.flutter0.dev/flutter_infra_release/flutter/${{ steps.engine_content_hash.outputs.value }}/engine_stamp.json"',
    );
    run.writeln(r'if curl --head --silent --fail "$URL" > /dev/null; then');
    run.writeln(r'  echo "Engine stamp exists at $URL"');
    run.writeln(r'  echo "should_run=false" >> $GITHUB_OUTPUT');
    run.writeln(r'else');
    run.writeln(r'  echo "Engine stamp does not exist at $URL"');
    run.writeln(r'  echo "should_run=true" >> $GITHUB_OUTPUT');
    run.writeln(r'fi');
  }
  {
    final step = steps.beginMap('name', 'Fetch latest content hash');
    step.write('id', 'fetch_latest_content_hash');
    final run = step.beginMap('run', '|');
    run.writeln(r'LATEST_URL="https://engine.flutter0.dev/flutter_infra_release/flutter/latest_content_hash.txt"');
    run.writeln(r'curl --fail -o latest_content_hash.txt $LATEST_URL');
    run.writeln(r'LATEST_HASH=$(cat latest_content_hash.txt)');
    run.writeln(r'echo "::notice:: Latest content hash: ${LATEST_HASH}"');
    run.writeln(r'echo "value=${LATEST_HASH}" >> $GITHUB_OUTPUT');
  }
}

void main(List<String> arguments) {
  final parser = ArgParser();
  parser.addMultiOption(
    'input',
    abbr: 'i',
    help: 'Path to the builder config JSON file.',
    valueHelp: 'path',
    defaultsTo: [],
  );
  parser.addOption(
    'output',
    abbr: 'o',
    help: 'Path to output YAML file. If not specified, output to stdout.',
    valueHelp: 'path',
    defaultsTo: '',
  );
  parser.addMultiOption(
    'artifact-tests',
    abbr: 't',
    help: 'Path to a JSON file with artifact test job specs (test_jobs).',
    valueHelp: 'path',
    defaultsTo: [],
  );

  final ArgResults args;
  try {
    args = parser.parse(arguments);
  } on FormatException catch (e) {
    print('Error parsing arguments: ${e.message}');
    print('Usage:\n${parser.usage}');
    exit(1);
  }

  final input = args['input'] as List<String>;
  if (input.isEmpty) {
    print('No input files specified. Use --input to specify at least one builder config JSON file.');
    exit(1);
  }

  final yamlWriter = YamlWriter();
  final artifactTestSpecs = (args['artifact-tests'] as List<String>?) ?? const <String>[];
  final artifactTestJobs = _readArtifactTestJobs(artifactTestSpecs);
  final testBuildOutputs = artifactTestJobs.expand((job) => job.downloads).toSet();
  final root = yamlWriter.root;
  root.writeln('# This file is generated through `scripts/update_github_workflow.sh.`');
  root.writeln('# Do not edit directly.');
  root.write('name', 'Engine Artifacts');

  final on = root.beginMap('on');
  final push = on.beginMap('push');
  push.write('branches', '["staging"]');
  on.beginMap('pull_request');

  final env = root.beginMap('env');
  env.write('DEPOT_TOOLS_WIN_TOOLCHAIN', '0');
  env.write('FLUTTER_PREBUILT_DART_SDK', '1');
  env.write('ENGINE_CHECKOUT_PATH', r'${{ github.workspace }}/engine');

  final jobs = root.beginMap('jobs');

  _writeGuardJob(jobs);

  final artifactPublisher = ArtifactPublisher();

  for (final inputPath in args['input'] as List<String>) {
    final content = File(inputPath).readAsStringSync();
    final map = jsonDecode(content) as Map<String, dynamic>;
    final buildConfig = BuilderConfig.fromJson(path: inputPath, map: map);
    final writer = BuildConfigWriter(
      config: buildConfig,
      jobsSections: jobs,
      artifactPublisher: artifactPublisher,
      testBuildOutputs: testBuildOutputs,
    );
    writer.write();
  }

  _writeArtifactTestJobs(jobs, artifactPublisher, artifactTestJobs);

  artifactPublisher.writePublishJob(jobs);
  _writeSmokeTestInvocationJob(jobs);
  // The per-run prefix this run published to the CI bucket is removed
  // afterwards, unconditionally (always(), pass, fail, cancel - the run's
  // deployment is disposable).
  _writeCiBucketTeardownJob(jobs, const {'publish_artifacts', 'smoke_test'});

  if (args['output'] != '') {
    final outputFile = File(args['output'] as String);
    outputFile.writeAsStringSync(yamlWriter._buffer.toString());
  } else {
    print(yamlWriter._buffer.toString());
  }
}
