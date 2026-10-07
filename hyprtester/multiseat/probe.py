"""Two real seats in one fork process; never connects input to the host session."""
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
           GTK_IM_MODULE='wayland', GIO_USE_VFS='local', GSETTINGS_BACKEND='memory')
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
hl.window_rule({name="interactive-seat-test", match={title="^constraints-window$"}, float=true})
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
    ok('output create headless agent')
    ok('eval hl.monitor({output="agent",mode="1280x800",position="1280x0",scale=1})')
    for name in initial_outputs:
        ok('eval hl.monitor({output=' + json.dumps(name) + ',disabled=true})')
    wait_for(lambda: len(ctl('monitors', True)) == 2)
    ENV['WAYLAND_DISPLAY'] = wait_for(lambda: next((p.name for p in RT.glob('wayland-*') if p.is_socket()), None))
    ok('dispatch hl.dsp.focus({monitor="human"})')
    ok('dispatch hl.dsp.focus({workspace="1"})')
    start(['/usr/bin/python3', str(SOURCE / 'gtk-window.py'), 'human-window', str(BASE / 'human-text')], 'human-app')
    wait_for(lambda: any(c['title'] == 'human-window' for c in ctl('clients', True)))
    ok('seat create agent agent')
    agent_env = ENV.copy()
    agent_env['WAYLAND_DISPLAY'] = ctl('seat list', True)[0]['display']
    cli_seats = json.loads(subprocess.check_output(['hyprctl', '-j', 'seat', 'list'], env=ENV, text=True, timeout=5))
    assert cli_seats[0]['display'] == agent_env['WAYLAND_DISPLAY']
    record('hyprctl seat API', 'private instance verified')
    start(['/usr/bin/python3', str(SOURCE / 'gtk-window.py'), 'agent-window', str(BASE / 'agent-text')], 'agent-app', env=agent_env)
    wait_for(lambda: any(c['title'] == 'agent-window' for c in ctl('clients', True)))
    human_ime = start([str(BASE / 'ime')], 'human-ime', env=ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    agent_ime = start([str(BASE / 'ime')], 'agent-ime', env=agent_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    for process in (human_ime, agent_ime):
        assert select.select([process.stdout], [], [], 5)[0] and process.stdout.readline().strip() == 'ready'
    human = input_client('Hyprland', 'human', ENV)
    agent = input_client('agent', 'agent', agent_env)
    registry = subprocess.check_output([str(BASE / 'registry')], env=ENV, text=True)
    agent_registry = subprocess.check_output([str(BASE / 'registry')], env=agent_env, text=True)
    assert 'seat_name=Hyprland' in registry and 'seat_name=agent' not in registry
    assert 'seat_name=agent' in agent_registry and 'seat_name=Hyprland' not in agent_registry
    assert next(c for c in ctl('clients', True) if c['title'] == 'agent-window')['workspace']['id'] == 2
    (BASE / 'agent-registry.txt').write_text(agent_registry)
    (BASE / 'registry.txt').write_text(registry)
    record('registry', {'seats': [line for line in registry.splitlines() if line.startswith('seat_name=')], 'agent_seats': [line for line in agent_registry.splitlines() if line.startswith('seat_name=')], 'fork_pid': compositor.pid})
    command(human, 'motion 300 300')
    command(human, 'type HUMAN')
    before = state()
    command(agent, 'motion 350 330')
    command(agent, 'type AGENT')
    ok('seat workspace agent 3')
    ok('seat workspace agent 2')
    after = state()
    record('independent input and workspace', {'before': before, 'after': after, 'human_text': text('human-text'), 'agent_text': text('agent-text'), 'seat': ctl('seat list', True)})
    assert before == after, 'agent changed primary seat state'
    assert text('human-text') == 'HUMAN' and text('agent-text') == 'AGENT', 'input reached wrong application or was lost'
    if os.environ.get('MULTISEAT_EXTRA_SEATS'):
        many_before = state()
        extra = []
        for index in (1, 2):
            name = 'extra' + str(index)
            ok('eval hl.workspace_rule({workspace=\"' + str(200 + index) + '\",monitor=\"' + name + '\",default=true})')
            ok('output create headless ' + name)
            ok('eval hl.monitor({output="' + name + '",mode="1280x800",position="' + str((index + 1) * 1280) + 'x0",scale=1})')
            wait_for(lambda: any(m['name'] == name for m in ctl('monitors', True)))
            ok('seat create ' + name + ' ' + name)
            env = {**ENV, 'WAYLAND_DISPLAY': next(s['display'] for s in ctl('seat list', True) if s['name'] == name)}
            if index == 2: env['MULTISEAT_NULL_POINTER_SEAT'] = '1'
            ok('seat workspace ' + name + ' ' + str(200 + index))
            app = start(['/usr/bin/python3', str(SOURCE / 'gtk-window.py'), name + '-window', str(BASE / (name + '-text'))], name + '-app', env=env)
            wait_for(lambda: any(c['title'] == name + '-window' for c in ctl('clients', True)))
            driver = input_client(name, name, env)
            wait_for(lambda: text(name + '-text.ready') == 'focused')
            command(driver, 'type EXTRA' + str(index))
            extra.append((name, app, driver))
        def many_type(driver, character):
            for i in range(20):
                command(driver, 'motion 300 300')
                command(driver, 'type ' + character)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = [pool.submit(many_type, human, 'H'), pool.submit(many_type, agent, 'A')]
            jobs += [pool.submit(many_type, extra[0][2], 'X'), pool.submit(many_type, extra[1][2], 'Y')]
            for job in jobs: job.result()
        assert text('human-text') == 'HUMAN' + 'H' * 20
        assert text('agent-text') == 'AGENT' + 'A' * 20
        assert text('extra1-text') == 'EXTRA1' + 'X' * 20
        assert text('extra2-text') == 'EXTRA2' + 'Y' * 20
        assert next(s['cursor'] for s in ctl('seat list', True) if s['name'] == 'extra2') == [4140, 300]
        assert state() == many_before
        before = state()
        for name, app, driver in extra:
            ok('seat workspace ' + name + ' name:' + name + '-alternate')
        assert state() == before
        record('four simultaneous seats', {'human': text('human-text'), 'agents': [text('agent-text'), text('extra1-text'), text('extra2-text')], 'seats': ctl('seat list', True), 'primary_state_unchanged': True, 'null_pointer_seat_routed_by_connection': True})
        for driver, initial in ((human, 'HUMAN'), (agent, 'AGENT')):
            command(driver, 'mods 4')
            command(driver, 'key 30 1'); command(driver, 'key 30 0')
            command(driver, 'mods 0')
            command(driver, 'key 14 1'); command(driver, 'key 14 0')
            command(driver, 'type ' + initial)
        for name, app, driver in reversed(extra):
            driver.stdin.close()
            assert driver.wait(timeout=5) == 0
            app.terminate(); app.wait(timeout=5)
            wait_for(lambda: not any(c['title'] == name + '-window' for c in ctl('clients', True)))
            ok('seat remove ' + name)
            ok('output remove ' + name)
    for round_number in range(20):
        command(human, 'motion ' + str(300 + round_number) + ' 300')
        command(human, 'type H')
        before = state()
        command(agent, 'motion ' + str(350 + round_number) + ' 330')
        command(agent, 'type A')
        ok('seat workspace agent 3')
        ok('seat workspace agent 2')
        assert state() == before, 'cross-seat state mutation'
    record('interleaved input', {'rounds': 20, 'human_text': text('human-text'), 'agent_text': text('agent-text')})
    def simultaneous(process, character, base_x):
        for round_number in range(20):
            command(process, 'motion ' + str(base_x + round_number) + ' 300')
            command(process, 'type ' + character)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(simultaneous, human, 'H', 300), pool.submit(simultaneous, agent, 'A', 350)]
        for future in futures:
            future.result()
    assert text('human-text') == 'HUMAN' + 'H' * 40
    assert text('agent-text') == 'AGENT' + 'A' * 40
    record('simultaneous input', {'rounds_per_seat': 20, 'exact_text': True})
    for primary in (False, True):
        flag = ['--primary'] if primary else []
        for label, env in (('HUMAN-CLIPBOARD', ENV), ('AGENT-CLIPBOARD', agent_env)):
            subprocess.run(['wl-copy', *flag, label], env=env, check=True, timeout=5)
        for expected, env in (('HUMAN-CLIPBOARD', ENV), ('AGENT-CLIPBOARD', agent_env)):
            actual = subprocess.check_output(['wl-paste', '--no-newline', *flag], env=env, text=True, timeout=5)
            assert actual == expected, (expected, actual)
        record('primary selection' if primary else 'data-control clipboard', 'isolated')
    def shortcut(process, code):
        command(process, 'mods 4')
        command(process, 'key ' + str(code) + ' 1')
        command(process, 'key ' + str(code) + ' 0')
        command(process, 'mods 0')
    expected_human, expected_agent = text('human-text'), text('agent-text')
    for process in (human, agent):
        shortcut(process, 30)  # Ctrl+A
        shortcut(process, 46)  # Ctrl+C through wl_data_device
    for process in (human, agent):
        command(process, 'key 14 1')
        command(process, 'key 14 0')
        shortcut(process, 47)  # Ctrl+V
    assert text('human-text') == expected_human
    assert text('agent-text') == expected_agent
    record('GTK copy and paste', 'isolated')
    for process, label in ((human, 'human-text'), (agent, 'agent-text')):
        command(process, 'motion 640 230')
        command(process, 'button 272 1')
        command(process, 'button 272 0')
        assert text(label + '.click') == 'click\n'
        command(process, 'motion 600 137')
        command(process, 'button 272 1')
        command(process, 'button 272 0')
        command(process, 'key 107 1')  # End
        command(process, 'key 107 0')
    assert text('human-text.click') == text('agent-text.click') == 'click\n'
    record('pointer click recipients', 'isolated')
    command(agent, 'mods 1')
    before_text = text('human-text')
    command(human, 'type lowercase')
    assert text('human-text') == before_text + 'lowercase'
    command(agent, 'mods 0')
    record('held Shift isolation', True)
    command(agent, 'button 273 1')
    command(agent, 'button 273 0')
    wait_for(lambda: text('agent-text.popup'))
    time.sleep(.5)
    before = state()
    before_text = text('human-text')
    command(human, 'type MENU')
    assert text('human-text') == before_text + 'MENU' and state() == before
    subprocess.run(['grim', '-o', 'agent', str(BASE / 'agent-popup.png')], env=agent_env, check=True, timeout=10)
    command(agent, 'key 1 1')
    command(agent, 'key 1 0')
    record('agent popup grab while human types', True)
    subprocess.run(['grim', '-o', 'agent', str(BASE / 'agent-before-drag.png')], env=agent_env, check=True, timeout=10)
    before_text = text('human-text')
    command(agent, 'motion 600 325')
    command(agent, 'button 272 1')
    command(agent, 'motion 640 350')
    command(human, 'type DRAG')
    command(agent, 'motion 600 420')
    time.sleep(.5)
    command(agent, 'motion 620 418')
    time.sleep(.5)
    command(agent, 'button 272 0')
    subprocess.run(['grim', '-o', 'agent', str(BASE / 'agent-after-drag.png')], env=agent_env, check=True, timeout=10)
    wait_for(lambda: text('agent-text.drop'), 5)
    assert text('agent-text.drop') == 'agent-window-payload'
    assert not text('human-text.drop') and text('human-text') == before_text + 'DRAG'
    record('real GTK drag while human types', True)
    command(agent, 'motion 600 137')
    command(agent, 'button 272 1')
    command(agent, 'button 272 0')
    command(agent, 'key 107 1')
    command(agent, 'key 107 0')
    command(human_ime, '人类输入法')
    command(agent_ime, '代理输入法')
    assert text('human-text').endswith('人类输入法')
    assert text('agent-text').endswith('代理输入法')
    record('two real input methods into GTK entries', 'isolated')
    before_human, before_agent = text('human-text'), text('agent-text')
    for ime in (human_ime, agent_ime):
        command(ime, '@grab')
    command(agent, 'type ABC')
    command(human, 'type DEF')
    for ime in (human_ime, agent_ime):
        ime.stdin.write('@keys\n'); ime.stdin.flush()
        assert select.select([ime.stdout], [], [], 5)[0] and ime.stdout.readline().strip() == '3'
        command(ime, '@ungrab')
    assert text('human-text') == before_human and text('agent-text') == before_agent
    record('two simultaneous input-method keyboard grabs', 'isolated')
    lock = start([str(BASE / 'lock')], 'lock', env=ENV, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    assert select.select([lock.stdout], [], [], 10)[0] and lock.stdout.readline().strip() == 'ready'
    before_agent = text('agent-text')
    command(agent, 'motion 600 137')
    command(agent, 'button 272 1')
    command(agent, 'button 272 0')
    command(agent, 'type LOCKED')
    assert text('agent-text') == before_agent
    lock.stdin.write('keys\n'); lock.stdin.flush()
    assert select.select([lock.stdout], [], [], 5)[0] and lock.stdout.readline().strip() == '0'
    command(human, 'motion 300 300')
    command(human, 'type HUMANLOCK')
    lock.stdin.write('keys\n'); lock.stdin.flush()
    assert select.select([lock.stdout], [], [], 5)[0] and int(lock.stdout.readline().strip()) > 0
    command(lock, 'unlock')
    wait_for(lambda: lock.poll() is not None)
    command(agent, 'motion 600 137')
    command(agent, 'button 272 1')
    command(agent, 'button 272 0')
    command(agent, 'type UNLOCKED')
    assert text('agent-text') != before_agent and 'UNLOCKED' in text('agent-text')
    record('real session lock rejects agent, accepts human, restores agent', True)
    command(agent, 'release-seat')
    command(agent, 'type LIFETIME')
    assert text('agent-text').endswith('LIFETIME')
    record('wl_seat released before input children', True)
    for output in ('human', 'agent'):
        subprocess.run(['grim', '-o', output, str(BASE / (output + '.png'))], env=ENV if output == 'human' else agent_env, check=True, timeout=10)
    before = state()
    layer = start([str(BASE / 'layer')], 'agent-layer', env=agent_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    assert select.select([layer.stdout], [], [], 5)[0] and layer.stdout.readline().strip() == 'ready'
    command(layer, 'grab')
    command(agent, 'type LAYER')
    before_text = text('human-text')
    command(human, 'type HUMANLAYER')
    layer.stdin.write('keys\n'); layer.stdin.flush()
    assert select.select([layer.stdout], [], [], 5)[0] and layer.stdout.readline().strip() == '5'
    assert state() == before and text('human-text') == before_text + 'HUMANLAYER'
    subprocess.run(['grim', '-o', 'agent', str(BASE / 'agent-layer.png')], env=agent_env, check=True, timeout=10)
    command(layer, 'stop')
    assert layer.wait(timeout=5) == 0
    record('layer-shell and Hyprland focus grab', 'isolated')
    constraint = start([str(BASE / 'constraints')], 'agent-constraints', env=agent_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    assert select.select([constraint.stdout], [], [], 5)[0] and constraint.stdout.readline().strip() == 'ready'
    window = wait_for(lambda: next((c for c in ctl('clients', True) if c['title'] == 'constraints-window'), None))
    x, y = window['at']
    command(agent, 'motion ' + str(x - 1280 + 80) + ' ' + str(y + 80))
    before = state()
    command(agent, 'button 272 1')
    command(agent, 'button 272 0')
    command(agent, 'type TOKEN')
    command(constraint, 'activate')
    assert state() == before and ctl('seat list', True)[0]['window'] == 'constraints-window'
    record('agent xdg-activation token and focus', 'isolated')
    command(constraint, 'lock')
    locked_position = ctl('seat list', True)[0]['cursor']
    before = state()
    command(agent, 'relative 80 50')
    assert ctl('seat list', True)[0]['cursor'] == locked_position and state() == before
    constraint.stdin.write('relative\n'); constraint.stdin.flush()
    assert select.select([constraint.stdout], [], [], 5)[0] and int(constraint.stdout.readline().strip()) > 0
    command(constraint, 'unlock')
    command(agent, 'relative 80 50')
    assert ctl('seat list', True)[0]['cursor'] != locked_position
    command(constraint, 'confine')
    command(agent, 'motion 1270 790')
    px, py = ctl('seat list', True)[0]['cursor']
    assert x + 50 <= px <= x + 150 and y + 50 <= py <= y + 150, (window, px, py)
    assert state() == before
    command(constraint, 'unlock')
    record('locked pointer, relative motion and confinement', 'isolated')
    def constraint_window():
        return next(c for c in ctl('clients', True) if c['title'] == 'constraints-window')
    for action in ('move', 'resize'):
        window = constraint_window()
        x, y = window['at']
        command(agent, 'motion ' + str(x - 1280 + 80) + ' ' + str(y + 80))
        command(constraint, action)
        before = state()
        command(agent, 'button 272 1')
        command(agent, 'relative 80 50')
        command(human, 'type WINDOWDRAG')
        command(agent, 'relative 20 20')
        command(agent, 'button 272 0')
        changed = wait_for(lambda: constraint_window() if constraint_window()['at' if action == 'move' else 'size'] != window['at' if action == 'move' else 'size'] else None)
        assert state() == before and changed['monitor'] == window['monitor']
    subprocess.run(['grim', '-o', 'agent', str(BASE / 'agent-window-drag.png')], env=agent_env, check=True, timeout=10)
    record('client-initiated window move and resize', 'isolated')
    before = state()
    command(agent, 'mods 1')
    command(agent, 'key 30 1')
    command(agent, 'button 272 1')
    ok('seat remove agent')
    command(agent, 'type REMOVED')
    assert state() == before and not text('agent-text').endswith('REMOVED')
    record('retired seat cannot inject into primary', True)
    ok('seat create agent agent')
    new_display = ctl('seat list', True)[0]['display']
    assert new_display != agent_env['WAYLAND_DISPLAY']
    before = state()
    command(agent, 'relative 90 90')
    command(agent, 'key 30 0')
    command(agent, 'button 272 0')
    assert state() == before
    new_env = {**ENV, 'WAYLAND_DISPLAY': new_display}
    fresh = input_client('agent', 'agent', new_env)
    command(fresh, 'motion 400 700')
    assert ctl('seat list', True)[0]['cursor'] == [1680, 700]
    ok('seat remove agent')
    record('held input retirement and same-name recreation', 'old devices inert')
    fresh.stdin.close()
    assert fresh.wait(timeout=5) == 0
    for cycle in range(3):
        ok('seat create agent agent')
        env = {**ENV, 'WAYLAND_DISPLAY': ctl('seat list', True)[0]['display']}
        temporary_input = input_client('agent', 'agent', env)
        command(temporary_input, 'motion 300 700')
        ok('seat remove agent')
        temporary_input.stdin.close()
        assert temporary_input.wait(timeout=5) == 0
    record('disconnected seat collection', 'three cycles passed')
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
