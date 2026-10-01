#!/usr/bin/env python3
"""Smoke test for the headless flutter_zero engine.

Runs a sample project (created with the in-tree flutter tool) against the
local engine and asserts on the output it prints: launch, hot reload (after
modifying the source), hot restart, quit.

    python3 dev/smoke_test/run.py --device [macos|windows|linux|ios|android] \
        --engine ci/host_debug [--host-engine ci/host_engine]

iOS simulators and Android emulators are booted when needed and closed after
the run.
"""
import argparse
import collections
import json
import os
import platform
import subprocess
import sys
import tempfile
import threading
import time

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
ENGINE_SRC_ROOT = os.path.join(REPO_ROOT, 'engine', 'src')
APP_SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'main.dart')
PROJECT_NAME = 'smoke_sample_project'
DEVICE_BOOT_TIMEOUT_S = 600.0
ANDROID_BOOT_TIMEOUT_S = 900.0
RELOAD_TIMEOUT_S = 120.0
QUIT_GRACE_S = 30.0
# The tool is the in-tree one of this checkout; all invocations (including
# the nested ones during the app build) use it.
FLUTTER_TOOL = os.path.join(
    REPO_ROOT, 'bin', 'flutter.bat' if platform.system() == 'Windows' else 'flutter')


def log(msg):
  print('SMOKE-HARNESS:%s' % msg, flush=True)


def die(tool_tail, msg):
  log('FAIL:%s' % msg)
  for line in tool_tail:
    if line is not None:
      log('tool:%s' % line.rstrip() if line.rstrip() else '')
  sys.exit(1)


def send_daemon_command(process, index, method, app_id, full_restart=False):
  """Sends one daemon command.

  `app.restart` covers both hot reload (fullRestart false) and hot restart
  (fullRestart true). Daemon lines are single-command json arrays with the
  appId in params."""
  payload = [{'method': method, 'id': index,
              'params': {'appId': app_id, 'fullRestart': full_restart}}]
  process.stdin.write(json.dumps(payload) + '\n')
  process.stdin.flush()


def write_app_file(project_dir, version):
  """Writes the versioned fixture as the sample project entrypoint.

  The version constant lets the harness prove that modified source (not just
  re-executed code) was loaded by a hot reload.
  """
  output = []
  with open(APP_SOURCE) as fixture:
    for line in fixture:
      output.append(line.replace("const String kVersion = 'A';",
                                 "const String kVersion = '%s';" % version))
  main_path = os.path.join(project_dir, 'lib', 'main.dart')
  with open(main_path, 'w') as main_file:
    main_file.write(''.join(output))
  return main_path


def create_project(workspace, platforms, engine, host_engine):
  log('creating sample project with the in-tree flutter tool')
  os.makedirs(workspace, exist_ok=True)
  # Create with the local engine configured as well, so that the tool caches
  # do not rely on downloadable sdk artifacts; CI may need to operate without
  # any published storage.
  local_engines = [] if engine is None else [
      '--local-engine=%s' % engine,
      '--local-engine-host=%s' % host_engine,
      '--local-engine-src-path=%s' % ENGINE_SRC_ROOT,
  ]
  code, output = run_tool_sync(
      ['create', '--platforms=%s' % platforms, PROJECT_NAME,
       '--suppress-analytics', *local_engines],
      cwd=workspace)
  if code != 0:
    die(output.splitlines()[-5:],
        '`flutter create` of the sample project failed')
  return os.path.join(workspace, PROJECT_NAME)


SIM_NAME = 'flutter_zero_smoke_test_sim'


def boot_ios_simulator():
  """Creates and boots a fresh iOS simulator; returns (udid, device flag)."""
  # Device lines look like: "iPhone 16 Pro (292C3A03-2D02-4548-9154-34A231D581C3)"
  # so the UDID is captured, not the trailing boot state.
  def _udid_of(state):
    return os.popen(
        'xcrun simctl list devices %s | grep -m1 iPhone | '
        'grep -Eo "[0-9A-F]{8}-[0-9A-F-]{27}"' % state).read().strip()
  udid = _udid_of('booted')
  if udid:
    log('found a booted iOS simulator: %s' % udid)
    return udid, None
  log('creating and booting the iOS simulator')
  udid = _udid_of('available')
  if not udid:
    die([], 'no available iOS simulator device found')
  subprocess.run(['xcrun', 'simctl', 'create', SIM_NAME, udid],
                 check=False, stdout=subprocess.PIPE)
  boot = subprocess.run(
      ['xcrun', 'simctl', 'boot', udid],
      check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
      text=True)
  if boot.returncode != 0 and 'already booted' not in boot.stderr.lower():
    die([], 'booting the iOS simulator failed')
  try:
    subprocess.run(['xcrun', 'simctl', 'bootstatus', udid, '-b'],
                   check=False, stdout=subprocess.DEVNULL,
                   timeout=DEVICE_BOOT_TIMEOUT_S)
  except subprocess.TimeoutExpired:
    die([], 'iOS simulator did not boot in %ss' % DEVICE_BOOT_TIMEOUT_S)
  return udid, SIM_NAME


def android_tools():
  """Returns the (emulator, adb, avdmanager) command paths of the Android SDK.

  Precedence: ANDROID_SDK_ROOT / ANDROID_HOME, then the platform defaults."""
  candidates = [
      os.environ.get('ANDROID_SDK_ROOT'),
      os.environ.get('ANDROID_HOME'),
      os.path.join(os.path.expanduser('~'), 'Library', 'Android', 'sdk'),
      os.path.join(os.path.expanduser('~'), 'Android', 'Sdk'),
      os.path.join(os.path.expanduser('~'), 'AppData', 'Local', 'Android',
                   'Sdk'),
  ]
  sdk = next((path for path in candidates if path and os.path.isdir(path)),
             None)
  if sdk is None:
    die([], 'Android SDK not found; set ANDROID_SDK_ROOT to its location')
  emulator, adb, avdmanager = (
      os.path.join(sdk, 'emulator', 'emulator'),
      os.path.join(sdk, 'platform-tools', 'adb'),
      os.path.join(sdk, 'cmdline-tools', 'latest', 'bin', 'avdmanager'),
  )
  missing = [tool for tool in (emulator, adb, avdmanager)
             if not os.path.exists(tool)]
  if missing:
    die([], 'Android SDK is missing tools: %s' % ', '.join(missing))
  return emulator, adb, avdmanager


def android_abi():
  """Returns the Android system image ABI for the host architecture."""
  return {'arm64': 'arm64-v8a'}.get(platform.machine(), 'x86_64')


# SDK packages that smoke test needs to boot an emulator on CI, where the
# runner image does not preinstall them with the Android SDK.
ANDROID_SDK_IMAGE = 'system-images;android-31;google_apis;%s' % android_abi()
ANDROID_EMULATOR_PACKAGES = ('emulator', ANDROID_SDK_IMAGE)


def _android_sdk_root():
  """Returns the Android SDK root directory, or None."""
  candidates = [
      os.environ.get('ANDROID_SDK_ROOT'),
      os.environ.get('ANDROID_HOME'),
      os.path.join(os.path.expanduser('~'), 'Library', 'Android', 'sdk'),
      os.path.join(os.path.expanduser('~'), 'Android', 'Sdk'),
      os.path.join(os.path.expanduser('~'), 'AppData', 'Local', 'Android',
                   'Sdk'),
      '/usr/local/lib/android/sdk',
  ]
  return next((path for path in candidates if path and os.path.isdir(path)),
              None)


def _install_android_emulator_packages():
  """Installs the Android emulator and AVD system image when missing.

  CI runner images ship the Android SDK without the emulator package and
  system images required to boot an AVD."""
  sdk = _android_sdk_root()
  if sdk is None:
    die([], 'Android SDK not found; set ANDROID_SDK_ROOT to its location')
  system_image_path = os.path.join(sdk, *filter(None, ANDROID_SDK_IMAGE.split(';')))
  missing = []
  if not os.path.isdir(os.path.join(sdk, 'emulator')):
    missing.append('emulator')
  if not os.path.isdir(system_image_path):
    missing.append(ANDROID_SDK_IMAGE)
  if not missing:
    return
  sdkmanager = os.path.join(
      sdk, 'cmdline-tools', 'latest', 'bin',
      'sdkmanager.bat' if platform.system() == 'Windows' else 'sdkmanager')
  if not os.path.isfile(sdkmanager):
    die([], 'the Android SDK is missing sdkmanager at %s' % sdkmanager)
  log('installing Android SDK packages: %s' % ', '.join(missing))
  result = subprocess.run(
      [sdkmanager if platform.system() != 'Windows'
       else 'cmd', '--install'] + missing,
      input='y\n' * 64,  # accept the licenses
      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
      check=False, env={**os.environ, 'ANDROID_SDK_ROOT': sdk})
  if result.returncode != 0:
    die(result.stdout.splitlines()[-8:],
        'installing Android SDK packages failed')


def boot_android_emulator():
  _install_android_emulator_packages()
  emulator, adb, avdmanager = android_tools()
  device = subprocess.run(
      [adb, 'devices'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
      text=True, check=False).stdout.strip().splitlines()
  emulators = [line.split()[0] for line in device
               if 'emulator' in line and 'daemon' not in line]
  if emulators:
    log('found a running Android emulator: %s' % emulators[0])
    return emulators[0], None
  log('creating the Android AVD')
  avd = 'flutter_zero_smoke_test_avd'
  result = subprocess.run(
      [avdmanager, 'create', 'avd', '-n', avd,
       '-k', 'system-images;android-31;google_apis;%s' % android_abi()],
      input='no\n',
      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
      check=False)
  tool_output = (result.stdout or '') + (result.stderr or '')
  avd_ready = result.returncode == 0 or 'already exists' in tool_output
  if not avd_ready:
    die([], 'avdmanager failed; is the Android SDK installed?')
  log('checking the Android emulator acceleration')
  accel = subprocess.run(
      [emulator, '-accel-check'], stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT, text=True)
  if accel.returncode != 0:
    # Software-only emulation boots far too slowly for CI; fail fast with the
    # probe's output, which points at the missing permissions (hosted runners
    # expose /dev/kvm but the runner user needs the udev rule from the
    # workflow's 'Enable KVM group perms' step).
    die([accel.stdout, accel.stderr],
        'the Android emulator cannot use hardware acceleration (KVM/HVF)')
  emulator_tail = collections.deque(maxlen=10)

  def _log_emulator():
    """Drains the emulator's output continuously and logs it.

    Without a reader, an emulator crash (e.g. a missing hypervisor) would go
    unnoticed while `adb wait-for-device` spins until the boot timeout; the
    CI log must show the reason and the final output lines on failure.
    """
    for line in emulator_process.stdout:
      text = line.rstrip()
      if text:
        log('emulator:%s' % text)
        emulator_tail.append(text)

  log('booting the Android emulator (headless, software rendering)')
  emulator_process = subprocess.Popen(
      [emulator, '-avd', avd,
       '-no-window', '-gpu', 'swiftshader_indirect',
       '-no-snapshot', '-no-audio'],
      stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
  threading.Thread(target=_log_emulator, daemon=True).start()
  try:
    subprocess.run(
        [adb, 'wait-for-device', 'shell', 'getprop', 'sys.boot_completed'],
        check=False, timeout=ANDROID_BOOT_TIMEOUT_S)
  except subprocess.TimeoutExpired:
    die(list(emulator_tail), 'Android emulator did not boot in %ss' % ANDROID_BOOT_TIMEOUT_S)
  device = subprocess.run(
      [adb, 'devices'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
      text=True, check=False).stdout.strip().splitlines()
  emulators = [line.split()[0] for line in device
               if 'emulator' in line and 'daemon' not in line]
  if not emulators:
    die([], 'no running Android emulator found after boot attempt')
  return emulators[0], emulator_process


def launch_flutter_run(project_dir, device, engine, host_engine, mode='debug'):
  build_mode_flags = {'profile': ['--profile'], 'release': ['--release']}.get(
      mode, [])
  # engine=None means: run against the prebuilt engine in bin/cache, without
  # any --local-engine flags.
  local_engine_flags = [] if engine is None else [
      '--local-engine=%s' % engine,
      '--local-engine-host=%s' % host_engine,
      '--local-engine-src-path=%s' % ENGINE_SRC_ROOT,
  ]
  local_web_sdk_flags = [] if engine is None else [
      '--local-web-sdk=%s' % engine,
      '--local-engine-src-path=%s' % ENGINE_SRC_ROOT,
  ]
  device_flag = {'web': 'chrome'}.get(device, device)
  args = [
      FLUTTER_TOOL, 'run', '--machine', '--verbose',
      '--suppress-analytics', '--no-version-check',
      *(local_web_sdk_flags if device == 'web' else local_engine_flags),
      '-d', device_flag,
  ] + build_mode_flags
  log('flutter run args: %s' % args)
  return subprocess.Popen(
      args, cwd=project_dir,
      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT, text=True, bufsize=1)


def stop_tool(process):
  if process.poll() is None:
    try:
      send_daemon_command(process, 0, 'app.stop', None)
      process.wait(timeout=QUIT_GRACE_S)
    except (subprocess.TimeoutExpired, OSError):
      process.terminate()


class ToolConsoleOutput:
  """Watches the console of a flutter tool that is running the sample app.

  The tool's stdout is consumed continuously (so the tool cannot deadlock
  on a full pipe), every line is echoed to the harness log, and
  daemon-protocol lines are scanned for the app.start event to learn the
  running app's id, which callers need for app.restart/app.stop commands.

  expect() waits until a console line matching the predicate shows up and
  returns it; on timeout (or when the tool exits first) it fails the run
  with a tail of the recent console lines.
  """

  def __init__(self, process):
    self._process = process
    self._lines = []
    self._tail = collections.deque(maxlen=30)
    self.app_id = None
    self._available = threading.Condition()
    self._done = False
    self._reader = threading.Thread(target=self._pump, daemon=True)
    self._reader.start()

  def _pump(self):
    for line in self._process.stdout:
      text = line.rstrip('\r\n')
      log('run:%s' % text)
      try:
        payload = json.loads(text)  # daemon lines are single-element arrays
        message = payload[0] if isinstance(payload, list) else payload
      except ValueError:
        message = None
      if isinstance(message, dict) and message.get('event') == 'app.start':
        app_id = (message.get('params') or {}).get('appId')
        if app_id:
          with self._available:
            self.app_id = app_id
      with self._available:
        self._tail.append(text)
        self._lines.append(text)
        self._available.notify_all()
    with self._available:
      self._done = True
      self._available.notify_all()

  def expect(self, predicate, timeout):
    """Waits up to timeout seconds for a console line matching predicate."""
    scanned = len(self._lines)
    deadline = time.time() + timeout
    while True:
      with self._available:
        for line in self._lines[scanned:]:
          if predicate(line):
            return line
        if self._done:
          break
        remaining = deadline - time.time()
        if remaining <= 0:
          break
        self._available.wait(min(0.5, remaining))
    die(list(self._tail)[-12:],
        'expected app console line did not appear within %ss' % timeout)


def run_debug_flow(project_dir, device, engine, host_engine):
  """Launch, hot reload (with modified source), hot restart, quit."""
  process = launch_flutter_run(project_dir, device, engine, host_engine)
  output = ToolConsoleOutput(process)
  command_index = 1
  try:
    output.expect(lambda line: 'SMOKE:boot:A:' in line, DEVICE_BOOT_TIMEOUT_S)
    log('launch ok')
    time.sleep(3.0)
    log('modifying lib/main.dart (A -> B)')
    write_app_file(project_dir, 'B')
    log('requesting hot reload')
    send_daemon_command(process, command_index, 'app.restart',
                        output.app_id, full_restart=False)
    command_index += 1
    line = output.expect(
        lambda line: 'SMOKE:tick:B:' in line, RELOAD_TIMEOUT_S)
    log('hot reload ok (%s)' % line.strip())
    log('requesting hot restart')
    send_daemon_command(process, command_index, 'app.restart',
                        output.app_id, full_restart=True)
    command_index += 1
    if device != 'web':
      # The web engine's hot restart hooks are dummies (no invocation), so the
      # pre-restart listener sentinel is only asserted on native devices.
      output.expect(
          lambda line: 'SMOKE:pre-restart:' in line, RELOAD_TIMEOUT_S)
    output.expect(
        lambda line: 'SMOKE:boot:B:0' in line, RELOAD_TIMEOUT_S)
    log('hot restart ok')
    send_daemon_command(process, command_index, 'app.stop', output.app_id)
    process.wait(timeout=QUIT_GRACE_S)
  finally:
    stop_tool(process)


def run_tool_sync(args, cwd=None):
  """Runs the in-tree tool synchronously, streaming all of its output.

  Every line is echoed to the CI log as it is produced (the full output
  remains available to the caller for failure reporting).
  """
  process = subprocess.Popen(
      [FLUTTER_TOOL] + args,
      cwd=cwd if cwd is not None else REPO_ROOT,
      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
      text=True, bufsize=1)
  lines = []
  for line in process.stdout:
    lines.append(line)
    log('tool:%s' % line.rstrip())
  process.wait()
  return process.returncode, ''.join(lines)


def run_boot_only_flow(project_dir, device, engine, host_engine, mode):
  """profile/release posture: verify the released compile, no console checks.

  For web the app is built with `flutter build web` and its artifacts are
  asserted (the released runtime has no console plumbing to forward); native
  devices run `flutter run --profile/--release` and assert the boot sentinel.
  """
  if device == 'web':
    code, output = run_tool_sync(
        ['build', 'web', '--verbose',
         '--suppress-analytics', '--no-version-check',
         '--local-web-sdk=%s' % engine,
         '--local-engine-src-path=%s' % ENGINE_SRC_ROOT,
         *( ['--profile'] if mode == 'profile' else ['--release'])],
        cwd=project_dir)
    if code != 0:
      die(output.splitlines()[-20:], 'web %s build failed' % mode)
    build_dir = os.path.join(project_dir, 'build', 'web')
    missing = [artifact for artifact in ('flutter.js', 'flutter_service_worker.js')
               if not os.path.exists(os.path.join(build_dir, artifact))]
    if missing:
      die([], 'web %s build is missing artifacts: %s' % (mode, ', '.join(missing)))
    log('build ok (%s)' % mode)
    return
  process = launch_flutter_run(project_dir, device, engine, host_engine, mode)
  output = ToolConsoleOutput(process)
  try:
    output.expect(lambda line: 'SMOKE:boot:A:' in line, DEVICE_BOOT_TIMEOUT_S)
    log('launch ok (%s)' % mode)
    time.sleep(3.0)
    send_daemon_command(process, 0, 'app.stop', output.app_id)
    process.wait(timeout=QUIT_GRACE_S)
  finally:
    stop_tool(process)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      '--device', required=True,
      choices=['macos', 'windows', 'linux', 'ios', 'android', 'web'])
  parser.add_argument(
      '--mode', default='debug', choices=['debug', 'profile', 'release'])
  parser.add_argument(
      '--engine', default=None,
      help='name of the local engine build the app runs against '
           '(default: run against the prebuilt engine in bin/cache '
           'without --local-engine flags).')
  parser.add_argument(
      '--host-engine', default=None,
      help='name of the local host engine the tool runs against '
           '(default: same as --engine).')
  parser.add_argument(
      '--workspace', default=None,
      help='where the sample project is created and left behind.')
  args = parser.parse_args()
  if args.engine is not None and args.host_engine is None:
    # The tool validates the host engine relative to the target out dir, so
    # the host variant is named without the "ci/" prefix.
    args.host_engine = args.engine[len('ci/'):] if args.engine.startswith('ci/') else args.engine
  device = args.device
  simulator = None
  emulator = None
  if args.device == 'ios':
    device, simulator = boot_ios_simulator()
  elif args.device == 'android':
    device, emulator = boot_android_emulator()
  platforms = args.device
  workspace = (args.workspace if args.workspace is not None
               else tempfile.mkdtemp(prefix='flutter_zero_smoke_test_'))
  try:
    project_dir = create_project(workspace, platforms, args.engine,
                                 args.host_engine)
    write_app_file(project_dir, 'A')
    if args.mode == 'debug':
      run_debug_flow(project_dir, device, args.engine, args.host_engine)
    else:
      run_boot_only_flow(project_dir, device, args.engine, args.host_engine,
                         args.mode)
    log('sample project left at %s' % workspace)
  finally:
    if args.device == 'ios' and simulator is not None:
      subprocess.run(['xcrun', 'simctl', 'shutdown', SIM_NAME],
                     check=False, stdout=subprocess.DEVNULL)
      subprocess.run(['xcrun', 'simctl', 'delete', SIM_NAME],
                     check=False, stdout=subprocess.DEVNULL)
    if emulator is not None and emulator.poll() is None:
      emulator.terminate()


if __name__ == '__main__':
  main()
