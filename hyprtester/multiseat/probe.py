"""Shared output, independent workspace views; real clients in a private compositor."""
import concurrent.futures
import json
import os
import pathlib
import select
import signal
import socket
import subprocess
import time
import tempfile
from gi.repository import GdkPixbuf

SOURCE = pathlib.Path(__file__).parent
BASE = pathlib.Path(os.environ['MULTISEAT_TEST_DIR'])
RT = pathlib.Path(tempfile.mkdtemp(prefix='hms-'))
PROCESSES = []
RESULTS = []
BUS_PID = None


def record(name, observed):
    RESULTS.append({'test': name, 'observed': observed})
    print(name, json.dumps(observed, ensure_ascii=False), flush=True)


def wait_for(callback, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = callback()
            if value:
                return value
        except (OSError, ValueError):
            pass
        time.sleep(.1)
    raise RuntimeError('readiness timeout')


ENV = os.environ.copy()
for key in ('HYPRLAND_INSTANCE_SIGNATURE', 'WAYLAND_SOCKET', 'DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'):
    ENV.pop(key, None)
ENV.update(XDG_RUNTIME_DIR=str(RT), XDG_CONFIG_HOME=str(BASE / 'config'), XDG_CACHE_HOME=str(BASE / 'cache'),
           XDG_STATE_HOME=str(BASE / 'state'), GDK_BACKEND='wayland', NO_AT_BRIDGE='1',
           GTK_IM_MODULE='wayland', GCOV_PREFIX=str(BASE / 'coverage'), GIO_USE_VFS='local', GSETTINGS_BACKEND='memory')
for name in ('config', 'cache', 'state'):
    (BASE / name).mkdir()


def start(command, name, **kwargs):
    output = kwargs.pop('stdout', open(BASE / (name + '.log'), 'w'))
    process = subprocess.Popen(command, env=kwargs.pop('env', ENV), stdout=output,
                               stderr=open(BASE / (name + '.stderr'), 'w'), start_new_session=True, **kwargs)
    PROCESSES.append(process)
    return process


def ctl(command, as_json=False):
    path = RT / 'hypr' / ENV['HYPRLAND_INSTANCE_SIGNATURE'] / '.socket.sock'
    assert path.is_relative_to(RT)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(path))
        connection.sendall((('j' if as_json else '') + '/' + command).encode())
        chunks = []
        while data := connection.recv(65536):
            chunks.append(data)
    text = b''.join(chunks).decode().strip()
    return json.loads(text) if as_json else text


def ok(command):
    answer = ctl(command)
    if answer != 'ok':
        raise RuntimeError(command + ': ' + answer)


def state():
    return {'cursor': ctl('cursorpos', True), 'window': ctl('activewindow', True).get('title'),
            'workspace': ctl('activeworkspace', True),
            'monitors': [(m['name'], m['activeWorkspace']) for m in ctl('monitors', True) if m['name'] == 'human']}


def command(process, text):
    process.stdin.write(text + '\n')
    process.stdin.flush()
    if not select.select([process.stdout], [], [], 5)[0]:
        raise RuntimeError('input command timeout: ' + text)
    answer = process.stdout.readline().strip()
    if answer != 'done':
        raise RuntimeError('input client failed: ' + answer)
    time.sleep(.06)


def input_client(seat, output, env):
    process = start([str(BASE / 'input'), seat, output], 'input-' + seat,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, env=env)
    if not select.select([process.stdout], [], [], 5)[0] or process.stdout.readline().strip() != 'ready':
        raise RuntimeError('input client not ready: ' + seat)
    return process


def text(name):
    path = BASE / name
    return path.read_text() if path.exists() else ''


def interrupted(signum, frame):
    raise KeyboardInterrupt('probe interrupted')


signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)

try:
    # Read host names only, before starting the private compositor; disable all
    # matching physical devices in its startup config, including no-op libseat.
    host_devices = json.loads(subprocess.check_output(['hyprctl', '-j', 'devices'], text=True, timeout=5))
    device_rules = []
    for group in ('mice', 'keyboards', 'touch'):
        for device in host_devices.get(group, []):
            device_rules.append('hl.device({name=' + json.dumps(device['name']) + ', enabled=false})')
    config = BASE / 'hyprland.lua'
    config.write_text('''hl.monitor({output="", mode="1280x800", position="auto", scale=1})
hl.workspace_rule({workspace="1", monitor="human", default=true})
hl.workspace_rule({workspace="2", monitor="agent", default=true})
hl.window_rule({name="human-seat-test", match={title="^human-window$"}, workspace="1"})
hl.window_rule({name="interactive-seat-test", match={title="^constraints-window$"}, float=true, workspace="10"})
hl.config({debug={enable_stdout_logs=true, disable_logs=false}, animations={enabled=false}, xwayland={enabled=false},
misc={disable_hyprland_logo=true, disable_splash_rendering=true, force_default_wallpaper=0}})
''' + '\n'.join(device_rules) + '\n')
    bus = subprocess.check_output(['dbus-daemon', '--session', '--fork', '--print-address=1', '--print-pid=1'], env=ENV, text=True).splitlines()
    ENV['DBUS_SESSION_BUS_ADDRESS'] = bus[0]
    BUS_PID = int(bus[1])
    start(['mutter', '--headless', '--wayland', '--no-x11', '--wayland-display=parent', '--virtual-monitor', '1280x800'], 'mutter')
    wait_for(lambda: (RT / 'parent').is_socket())
    ENV.update(WAYLAND_DISPLAY='parent', AQ_DRM_DEVICES='/dev/null', LIBSEAT_BACKEND='noop')
    compositor = start([os.environ['MULTISEAT_HYPRLAND'], '-c', str(config)], 'Hyprland')
    ENV['HYPRLAND_INSTANCE_SIGNATURE'] = wait_for(lambda: next((p.name for p in (RT / 'hypr').glob('*') if (p / '.socket.sock').is_socket()), None))
    wait_for(lambda: ctl('monitors', True) is not None)
    log = '\n'.join(p.read_text(errors='replace') for p in (RT / 'hypr' / ENV['HYPRLAND_INSTANCE_SIGNATURE']).glob('*.log'))
    record('private runtime', {'runtime': str(RT), 'no_drm': True, 'host_devices_disabled': len(device_rules)})
    assert 'drm: Starting backend' not in log and 'drm: Registered gpu' not in log
    errors = ctl('configerrors')
    assert not errors, errors
    initial_outputs = [m['name'] for m in ctl('monitors', True)]
    ok('output create headless human')
    ok('eval hl.monitor({output="human",mode="1280x800",position="0x0",scale=1})')
    for name in initial_outputs:
        ok('eval hl.monitor({output=' + json.dumps(name) + ',disabled=true})')
    wait_for(lambda: len(ctl('monitors', True)) == 1)
    ENV['WAYLAND_DISPLAY'] = wait_for(lambda: next((p.name for p in RT.glob('wayland-*') if p.is_socket()), None))
    ok('dispatch hl.dsp.focus({monitor="human"})')
    ok('dispatch hl.dsp.focus({workspace="1"})')
    human = input_client('Hyprland', 'human', ENV)
    agents = []
    for index in range(3):
        name = 'agent' + str(index + 1)
        ok('seat create ' + name + ' human')
        ok('seat workspace ' + name + ' ' + str(10 + index))
    start(['/usr/bin/python3', str(SOURCE / 'gtk-window.py'), 'human-window', str(BASE / 'human-text')], 'human-app')
    wait_for(lambda: any(c['title'] == 'human-window' for c in ctl('clients', True)))
    wait_for(lambda: text('human-text.ready') == 'focused')
    command(human, 'type HUMAN')
    before = state()
    for index in range(3):
        name = 'agent' + str(index + 1)
        env = {**ENV, 'MULTISEAT_SEAT': name, 'WAYLAND_DISPLAY': next(s['display'] for s in ctl('seat list', True) if s['name'] == name)}
        if index == 2: env['MULTISEAT_NULL_POINTER_SEAT'] = '1'
        driver = input_client(name, 'human', env)
        app = start(['/usr/bin/python3', str(SOURCE / 'gtk-window.py'), name + '-window', str(BASE / (name + '-text'))], name + '-app', env=env)
        wait_for(lambda: any(c['title'] == name + '-window' for c in ctl('clients', True)))
        wait_for(lambda: text(name + '-text.ready') == 'focused')
        command(driver, 'type AGENT' + str(index + 1))
        agents.append((name, env, driver, app))
    assert state() == before, (before, state())
    assert [c['workspace']['id'] for c in ctl('clients', True) if c['title'].startswith('agent')] == [10, 11, 12]
    record('four seats share one output with independent workspace views', {'primary_unchanged': True, 'seats': ctl('seat list', True)})
    for env in (ENV, agents[0][1]):
        registry = subprocess.check_output([str(BASE / 'registry')], env=env, text=True)
        assert all('seat_name=' + name in registry for name in ('Hyprland', 'agent1', 'agent2', 'agent3')), registry
    record('shared seat globals', 'primary and agent socket clients bind all seats')
    def many_type(driver, character):
        for i in range(20):
            command(driver, 'motion 300 300')
            command(driver, 'type ' + character)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        jobs = [pool.submit(many_type, human, 'H')]
        jobs += [pool.submit(many_type, agent[2], ch) for agent, ch in zip(agents, 'AXY')]
        for job in jobs: job.result()
    assert text('human-text') == 'HUMAN' + 'H' * 20
    for agent, ch in zip(agents, 'AXY'):
        assert text(agent[0] + '-text') == 'AGENT' + agent[0][-1] + ch * 20
    record('parallel real GTK input in hidden workspaces', 'all four exact text streams passed')
    before = state()
    captures = []
    for name in ('Hyprland', 'agent1', 'agent2', 'agent3'):
        path = BASE / (name + '.png')
        ok('seat capture ' + name + ' ' + str(path))
        assert path.stat().st_size > 10000
        captures.append(path.read_bytes())
    assert len(set(captures)) == 4
    assert state() == before
    record('seat workspace capture', {'four_different_frames': True, 'primary_unchanged': True})
    # Same workspace, independent keyboard focus. Cross-socket primary-launched window.
    agent = agents[0][2]
    ok('seat workspace agent1 1')
    command(agent, 'motion 600 137')
    command(agent, 'button 272 1')
    command(agent, 'button 272 0')
    command(agent, 'key 107 1')
    command(agent, 'key 107 0')
    command(agent, 'type SHARED')
    wait_for(lambda: text('human-text').endswith('SHARED'))
    assert text('agent1-text') == 'AGENT1' + 'A' * 20
    assert state() == before
    command(human, 'type ALSO')
    wait_for(lambda: text('human-text').endswith('SHAREDALSO'))
    ok('seat capture agent1 ' + str(BASE / 'shared agent view.png'))
    ok('seat capture Hyprland ' + str(BASE / 'shared-human-view.png'))
    record('shared native GTK window across socket origins', {'same_window_both_seats': text('human-text'), 'primary_focus_unchanged': True})
    ok('seat workspace agent1 10')
    command(agent, 'type BACKGROUND')
    wait_for(lambda: text('agent1-text').endswith('BACKGROUND'))
    ok('seat capture agent1 ' + str(BASE / 'agent1-updated.png'))
    assert (BASE / 'agent1-updated.png').read_bytes() != captures[1]
    # Real per-seat IME keyboard grabs (GTK's text-input context itself uses its default seat).
    imes = []
    for seat_name, env in (('Hyprland', ENV), ('agent1', agents[0][1])):
        ime = start([str(BASE / 'ime')], seat_name + '-ime', env={**env, 'MULTISEAT_SEAT': seat_name}, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        assert select.select([ime.stdout], [], [], 5)[0] and ime.stdout.readline().strip() == 'ready'
        command(ime, '@grab')
        imes.append(ime)
    human_before, agent_before = text('human-text'), text('agent1-text')
    command(human, 'type DEF'); command(agent, 'type ABC')
    for ime in imes:
        ime.stdin.write('@keys\n'); ime.stdin.flush()
        assert select.select([ime.stdout], [], [], 5)[0] and ime.stdout.readline().strip() == '3'
        command(ime, '@ungrab')
        ime.stdin.close(); assert ime.wait(timeout=5) == 0
    assert text('human-text') == human_before and text('agent1-text') == agent_before
    record('two independent real IME keyboard grabs', 'three keys per seat; no application or cross-seat delivery')
    # Independent clipboard sources and real payload drag on a hidden workspace.
    for primary in (False, True):
        flag = ['--primary'] if primary else []
        for label, env, seat_name in (('HUMAN-CLIP', ENV, 'Hyprland'), ('AGENT-CLIP', agents[0][1], 'agent1')):
            subprocess.run(['wl-copy', '--seat', seat_name, *flag, label], env=env, check=True, timeout=5)
        for expected, env, seat_name in (('HUMAN-CLIP', ENV, 'Hyprland'), ('AGENT-CLIP', agents[0][1], 'agent1')):
            actual = subprocess.check_output(['wl-paste', '--seat', seat_name, '--no-newline', *flag], env=env, text=True, timeout=5)
            assert actual == expected, (expected, actual)
    record('clipboard and primary selection', 'per-seat sources passed on shared output')
    command(agent, 'motion 600 325')
    command(agent, 'button 272 1')
    command(agent, 'motion 640 350')
    command(human, 'type DRAG')
    command(agent, 'motion 600 420')
    time.sleep(.5)
    command(agent, 'motion 620 418')
    time.sleep(.5)
    command(agent, 'button 272 0')
    wait_for(lambda: text('agent1-text.drop'), 5)
    assert text('agent1-text.drop') == 'agent1-window-payload'
    assert not text('human-text.drop') and text('human-text').endswith('DRAG')
    record('real GTK payload drag on hidden ws while human types', 'passed')
    ok('seat focus agent1 title:^agent1-window$')
    command(agent, 'motion 600 137')
    command(agent, 'button 272 1'); command(agent, 'button 272 0')
    ok('seat capture agent1 ' + str(BASE / 'agent1-pre-popup.png'))
    command(agent, 'button 273 1'); command(agent, 'button 273 0')
    wait_for(lambda: text('agent1-text.popup'))
    popup_before = state()
    command(human, 'type POPUP')
    assert state() == popup_before and text('human-text').endswith('POPUP')
    ok('seat capture agent1 ' + str(BASE / 'agent1-popup.png'))
    before_pixels = GdkPixbuf.Pixbuf.new_from_file(str(BASE / 'agent1-pre-popup.png')).get_pixels()
    after_pixels = GdkPixbuf.Pixbuf.new_from_file(str(BASE / 'agent1-popup.png')).get_pixels()
    changed_bytes = sum(a != b for a, b in zip(before_pixels, after_pixels))
    assert changed_bytes > 15000, ('popup missing from workspace capture', changed_bytes)
    command(agent, 'key 1 1'); command(agent, 'key 1 0')
    record('hidden workspace popup grab while human types', {'passed': True, 'capture_changed_bytes': changed_bytes})
    # A primary-origin client on hidden ws10 uses the requesting agent's seat.
    ok('seat workspace agent1 10')
    constraint = start([str(BASE / 'constraints')], 'cross-seat-constraints', env={**ENV, 'MULTISEAT_SEAT': 'agent1'}, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    assert select.select([constraint.stdout], [], [], 5)[0] and constraint.stdout.readline().strip() == 'ready'
    def constraint_window(): return next(c for c in ctl('clients', True) if c['title'] == 'constraints-window')
    window = wait_for(lambda: next((c for c in ctl('clients', True) if c['title'] == 'constraints-window'), None))
    command(agent, 'motion ' + str(window['at'][0] + 80) + ' ' + str(window['at'][1] + 80))
    command(agent, 'button 272 1'); command(agent, 'button 272 0')
    ok('dispatch hl.dsp.focus({window="title:^human-window$"})')
    constrained_before = state()
    command(human, 'type CONCURRENT')
    command(agent, 'type TOKEN'); command(constraint, 'activate')
    assert state() == constrained_before
    command(constraint, 'lock')
    locked_position = ctl('seat list', True)[0]['cursor']
    command(agent, 'relative 80 50')
    assert ctl('seat list', True)[0]['cursor'] == locked_position and state() == constrained_before
    constraint.stdin.write('relative\n'); constraint.stdin.flush()
    assert select.select([constraint.stdout], [], [], 5)[0] and int(constraint.stdout.readline().strip()) > 0
    command(constraint, 'unlock')
    command(constraint, 'confine'); command(agent, 'motion 1270 790')
    px, py = ctl('seat list', True)[0]['cursor']; x, y = window['at']
    assert x + 50 <= px <= x + 150 and y + 50 <= py <= y + 150
    command(constraint, 'unlock')
    for action in ('move', 'resize'):
        window = constraint_window(); x, y = window['at']
        command(agent, 'motion ' + str(x + 80) + ' ' + str(y + 80))
        command(constraint, action)
        command(agent, 'button 272 1'); command(agent, 'relative 80 50'); command(agent, 'button 272 0')
        after_window = wait_for(lambda: constraint_window() if constraint_window()['at' if action == 'move' else 'size'] != window['at' if action == 'move' else 'size'] else None)
        assert after_window['workspace']['id'] == 10, after_window
        assert state() == constrained_before, {'action': action, 'before': constrained_before, 'after': state()}
    record('cross-socket activation, pointer constraints and client move/resize', 'requesting agent seat preserved')
    # The same drag path must preserve the human's remembered focus on shared ws1.
    ok('dispatch hl.dsp.window.move({window="title:^constraints-window$",workspace="1",follow=false})')
    ok('seat workspace agent1 1')
    ok('dispatch hl.dsp.focus({window="title:^human-window$"})')
    shared_before = state()
    window = constraint_window(); x, y = window['at']
    command(agent, 'motion ' + str(x + 80) + ' ' + str(y + 80))
    command(constraint, 'move')
    command(agent, 'button 272 1'); command(agent, 'relative 80 50'); command(agent, 'button 272 0')
    wait_for(lambda: constraint_window()['at'] != window['at'])
    assert state() == shared_before, (shared_before, state())
    record('shared workspace client drag', 'human focus and workspace history unchanged')
    def child_key_count():
        constraint.stdin.write('keys\n'); constraint.stdin.flush()
        assert select.select([constraint.stdout], [], [], 5)[0]
        return int(constraint.stdout.readline().strip())
    count_before = child_key_count()
    command(constraint, 'release-seat')
    command(agent, 'type CHILDREN')
    assert child_key_count() == count_before + 8
    assert constraint.poll() is None and compositor.poll() is None
    record('native keyboard child survives wl_seat.release', 'serial routing retained')
    constraint.stdin.close(); assert constraint.wait(timeout=5) == 0
    ok('dispatch hl.dsp.focus({window="title:^human-window$"})')
    ok('seat workspace agent1 10')
    # Lock gates both input and screenshots without switching primary view.
    locker = start([str(BASE / 'lock'), 'human'], 'lock', stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    assert select.select([locker.stdout], [], [], 5)[0] and locker.stdout.readline().strip() == 'ready'
    wait_for(lambda: ctl('locked', True)['locked'])
    locked_text = text('agent1-text')
    command(agent, 'type BLOCKED')
    assert text('agent1-text') == locked_text
    assert ctl('seat capture agent1 ' + str(BASE / 'locked.png')) == 'session is locked'
    assert not (BASE / 'locked.png').exists()
    locker.stdin.write('unlock\n'); locker.stdin.flush()
    assert locker.wait(timeout=5) == 0
    wait_for(lambda: not ctl('locked', True)['locked'])
    record('session lock', 'agent input and capture blocked')
    # Clients on all sockets retain old seat children; retiring a seat cannot invalidate them.
    for name, env, driver, app in agents:
        command(driver, 'release-seat')
        command(driver, 'key 42 1'); command(driver, 'button 272 1')
        text_before = text(name + '-text')
        ok('seat remove ' + name)
        command(driver, 'type INERT')
        assert app.poll() is None and text(name + '-text') == text_before
        ok('seat create ' + name + ' human')
        replacement_before = next(s for s in ctl('seat list', True) if s['name'] == name)
        command(driver, 'type STALE'); command(driver, 'key 42 0'); command(driver, 'button 272 0')
        assert next(s for s in ctl('seat list', True) if s['name'] == name) == replacement_before
        assert text(name + '-text') == text_before
        assert compositor.poll() is None
        ok('seat remove ' + name)
    command(human, 'type STILL')
    assert text('human-text').endswith('STILL')
    assert len(ctl('clients', True)) == 4
    record('retirement with shared clients and live seat children', 'windows survive; stale input inert; primary functional')
    if os.environ.get('MULTISEAT_REGRESSION'):
        regression_env = ENV.copy()
        regression_env['WAYLAND_DISPLAY'] = 'parent'
        regression_env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        regression_env['PWD'] = str(SOURCE.parent)
        regression_config = BASE / 'regression.lua'
        regression_config.write_text('dofile(' + json.dumps(str(SOURCE.parent / 'test.lua')) + ')\n' + '\n'.join(device_rules) + '\n')
        regression = start([str(SOURCE.parent.parent / 'build-multiseat/hyprtester/hyprtester'),
                            '-c', str(regression_config), '-b', os.environ['MULTISEAT_HYPRLAND'],
                            '-p', str(SOURCE.parent / 'plugin/hyprtestplugin.so'),
                            'keyboardModifiersMergedOnFocus', 'pointerWarp', 'xdgInteractive',
                            'popupOpacityInheritsParentFade', 'xdgActivationSerial'],
                           'regression', env=regression_env, cwd=str(SOURCE.parent))
        assert regression.wait(timeout=180) == 0, 'single-seat integration regression failed'
        record('upstream single-seat integration', 'passed')
    record('result', 'passed')
except BaseException as error:
    record('failure', repr(error))
    raise
finally:
    for log_path in (RT / 'hypr').glob('*/*.log'):
        (BASE / (log_path.parent.name + '-' + log_path.name)).write_text(log_path.read_text(errors='replace'))
    (BASE / 'results.json').write_text(json.dumps(RESULTS, ensure_ascii=False, indent=2) + '\n')
    for process in reversed(PROCESSES):
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    for process in reversed(PROCESSES):
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=2)
    if BUS_PID:
        try:
            os.kill(BUS_PID, signal.SIGTERM)
        except ProcessLookupError:
            pass
    print('Evidence:', BASE, flush=True)
