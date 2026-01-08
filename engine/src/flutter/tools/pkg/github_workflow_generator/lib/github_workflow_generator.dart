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
    final step = steps.beginMap('name', 'Upload $outputPath/${path.basename(sourcePath)}');
    step.write('uses', 'actions/upload-artifact@v4');
    final w = step.beginMap('with');
    final id = _count++;
    final name = 'artifact_\${{ steps.engine_content_hash.outputs.value }}_$id';
    w.write('name', name);
    w.write('path', sourcePath);
    w.write('retention-days', '1');
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
    job.write('runs-on', 'ubuntu-latest');
    final steps = job.beginArray('steps');
    {
      final step = steps.beginMap('name', 'Checkout the repository');
      step.write('uses', 'actions/checkout@v4');
      final w = step.beginMap('with');
      w.write('path', "''");
    }
    {
      final step = steps.beginMap('name', 'Set up depot_tools');
      final run = step.beginMap('run', '|');
      run.writeln(r'git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $HOME/depot_tools');
      run.writeln('# Append depot_tools to the PATH for subsequent steps');
      run.writeln(r'echo "$HOME/depot_tools" >> $GITHUB_PATH');
    }
    {
      final step = steps.beginMap('name', 'Generate engine content hash');
      step.write('id', 'engine_content_hash');
      final run = step.beginMap('run', '|');
      run.writeln(r'engine_content_hash=$(bin/internal/content_aware_hash.sh)');
      run.writeln(r'echo "::notice:: Engine content hash: ${engine_content_hash}"');
      run.writeln(r'echo "value=${engine_content_hash}" >> $GITHUB_OUTPUT');
    }
    for (final artifact in _artifacts) {
      final name = path.basename(artifact.sourcePath);
      final step = steps.beginMap('name', 'Download ${artifact.outputPath}/$name');
      step.write('uses', 'actions/download-artifact@v4');
      final w = step.beginMap('with');
      w.write('name', artifact.artifactName);
      w.write('path', 'artifact-${artifact.id}/');
    }
    for (final artifact in _artifacts) {
      final name = path.basename(artifact.sourcePath);
      final step = steps.beginMap('name', 'Publish ${artifact.outputPath}/$name');
      step.write('uses', 'ryand56/r2-upload-action@b801a390acbdeb034c5e684ff5e1361c06639e7c');
      final w = step.beginMap('with');
      w.write('r2-account-id', r'${{ secrets.R2_ACCOUNT_ID }}');
      w.write('r2-access-key-id', r'${{ secrets.R2_ACCESS_KEY_ID }}');
      w.write('r2-secret-access-key', r'${{ secrets.R2_SECRET_ACCESS_KEY }}');
      w.write('r2-bucket', r'${{ env.R2_BUCKET }}');
      w.write('source-dir', 'artifact-${artifact.id}/');
      w.write('destination-dir', 'flutter_infra_release/flutter/\${{ steps.engine_content_hash.outputs.value }}/${artifact.outputPath}');
    }
  }

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
    final line = value.isEmpty ? '$label:' : '$label: $value';
    writeln(line);
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
  }) : _config = config,
       _jobsSections = jobsSections,
       _artifactPublisher = artifactPublisher;

  void write() {
    for (final build in _config.builds) {
      final job = _jobsSections.beginMap(_nameForBuild(build));
      job.write('runs-on', _getRunnerForBuilder(build));
      final defaults = job.beginMap('defaults');
      final run = defaults.beginMap('run');
      run.writeln('shell: bash');
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
      for (final generator in build.generators) {
        _writeBuildTask(steps, generator);
      }
      // For global generators we need to upload artifacts so that we can use them
      // in dependent jobs.
      if (_config.generators.isNotEmpty) {
        {
          final step = steps.beginMap('name', 'Upload build files');
          step.write('uses', 'actions/upload-artifact@v4');
          final w = step.beginMap('with');
          w.write('name', 'artifacts-${_nameForBuild(build)}-\${{ steps.engine_content_hash.outputs.value }}');
          w.write('path', 'engine/src/out/${build.name}');
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
      final jobSection = _jobsSections.beginMap(globalJobName);
      jobSection.write('runs-on', _getRunnerForBuilder(_config.builds.first));
      final needs = jobSection.beginArray('needs');
      for (final build in _config.builds) {
        needs.writeln(_nameForBuild(build));
      }
      final defaults = jobSection.beginMap('defaults');
      final run = defaults.beginMap('run');
      run.writeln('shell: bash');
      final steps = jobSection.beginArray('steps');
      _writePrelude(steps);
      for (final build in _config.builds) {
        final step = steps.beginMap('name', 'Download Artifacts from ${_nameForBuild(build)}');
        step.write('uses', 'actions/download-artifact@v4');
        final w = step.beginMap('with');
        w.write('name', 'artifacts-${_nameForBuild(build)}-\${{ steps.engine_content_hash.outputs.value }}');
        w.write('path', 'engine/src/out/${build.name}');
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
      final step = steps.beginMap('name', 'Set up depot_tools');
      step.write('if', "runner.os != 'Windows'");
      final run = step.beginMap('run', '|');
      run.writeln(r'git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $HOME/depot_tools');
      run.writeln('# Append depot_tools to the PATH for subsequent steps');
      run.writeln(r'echo "$HOME/depot_tools" >> $GITHUB_PATH');
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
      final step = steps.beginMap('name', 'Set up depot_tools');
      step.write('if', "runner.os == 'Windows'");
      final run = step.beginMap('run', '|');
      run.writeln(r'git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $HOME/depot_tools');
      run.writeln('# Append depot_tools to the PATH for subsequent steps');
      run.writeln(r'DEPOT_TOOLS_WIN=$(cygpath -w "$HOME/depot_tools")');
      run.writeln(r'echo "$DEPOT_TOOLS_WIN" >> $GITHUB_PATH');
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
      final step = steps.beginMap('name', 'Copy gclient file');
      final run = step.beginMap('run', '|');
      if (path.basename(_config.path).contains('android')) {
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
      return '../../bin/dart $script';
    } else if (language == 'python3') {
      return 'python3 $script';
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
  final root = yamlWriter.root;
  root.writeln('# This file is generated. Do not edit directly.');
  root.write('name', 'Engine Artifacts');

  final on = root.beginMap('on');
  on.beginMap('pull_request');

  final env = root.beginMap('env');
  env.write('DEPOT_TOOLS_WIN_TOOLCHAIN', '0');
  env.write('FLUTTER_PREBUILT_DART_SDK', '1');
  env.write('R2_BUCKET', 'flutter-zero-engine');
  env.write('ENGINE_CHECKOUT_PATH', r'${{ github.workspace }}/engine');

  final jobs = root.beginMap('jobs');

  final artifactPublisher = ArtifactPublisher();

  for (final inputPath in args['input'] as List<String>) {
    final content = File(inputPath).readAsStringSync();
    final map = jsonDecode(content) as Map<String, dynamic>;
    final buildConfig = BuilderConfig.fromJson(path: inputPath, map: map);
    final writer = BuildConfigWriter(
      config: buildConfig,
      jobsSections: jobs,
      artifactPublisher: artifactPublisher,
    );
    writer.write();
  }

  artifactPublisher.writePublishJob(jobs);

  if (args['output'] != '') {
    final outputFile = File(args['output'] as String);
    outputFile.writeAsStringSync(yamlWriter._buffer.toString());
  } else {
    print(yamlWriter._buffer.toString());
  }
}
