"""Generic compositor interfaces, independent of any desktop shell."""
import json, os, pathlib, signal, shlex, select, socket, subprocess, tempfile, time
FORK=pathlib.Path(__file__).resolve().parents[2]
ROOT=FORK
BASE=pathlib.Path(tempfile.mkdtemp(prefix='gc-'))
RT=BASE/'r';RT.mkdir(mode=0o700)
PROCESSES=[]
ENV=os.environ.copy()
for key in ('HYPRLAND_INSTANCE_SIGNATURE','WAYLAND_DISPLAY','WAYLAND_SOCKET','DISPLAY','DBUS_SESSION_BUS_ADDRESS'):
    ENV.pop(key,None)
ENV.update(XDG_RUNTIME_DIR=str(RT),XDG_CONFIG_HOME=str(BASE/'config'),XDG_CACHE_HOME=str(BASE/'cache'),XDG_STATE_HOME=str(BASE/'state'),GCOV_PREFIX=str(BASE/'coverage'),HYPRLAND_HEADLESS_ONLY='1',AQ_DRM_DEVICES='/dev/null',LIBSEAT_BACKEND='noop')
def wait(callback,timeout=25):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            value=callback()
            if value:return value
        except (OSError,ValueError,subprocess.SubprocessError):pass
        time.sleep(.1)
    raise RuntimeError('readiness timeout')
def ctl(command,as_json=False):
    path=RT/'hypr'/ENV['HYPRLAND_INSTANCE_SIGNATURE']/'.socket.sock'
    assert path.is_relative_to(RT)
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(4);connection.connect(str(path));connection.sendall((('j' if as_json else '')+'/'+command).encode())
        chunks=[]
        while data:=connection.recv(65536):chunks.append(data)
    answer=b''.join(chunks).decode().strip()
    return json.loads(answer) if as_json else answer
def ok(command):
    answer=ctl(command)
    assert answer=='ok',(command,answer)
def primary_state():
    monitors=ctl('monitors',True)
    return {'workspace':ctl('activeworkspace',True),'window':ctl('activewindow',True).get('address'),'cursor':ctl('cursorpos',True),'output':next(m for m in monitors if m['name']=='physical')['activeWorkspace']}
def record(name):print('PASS',name,flush=True)
def initialize():
    assert os.getenv('HYPRLAND_TEST_SANDBOX')=='1' and not pathlib.Path('/dev/input').exists(),'Use generic-controller.sh in its device sandbox'
    config=BASE/'hyprland.lua'
    config.write_text('hl.monitor({output="",mode="1280x800",position="auto",scale=1})\nhl.config({animations={enabled=false},xwayland={enabled=false},misc={disable_hyprland_logo=true,disable_splash_rendering=true,force_default_wallpaper=0},debug={enable_stdout_logs=true}})\n')
    bus=subprocess.Popen(['dbus-daemon','--session','--nofork','--print-address=1'],env=ENV,stdout=subprocess.PIPE,stderr=open(BASE/'bus.log','w'),text=True,start_new_session=True)
    PROCESSES.append(bus)
    ENV['DBUS_SESSION_BUS_ADDRESS']=bus.stdout.readline().strip()
    parent=subprocess.Popen(['mutter','--headless','--wayland','--no-x11','--wayland-display=parent','--virtual-monitor','1280x800'],env=ENV,stdout=open(BASE/'mutter.log','w'),stderr=subprocess.STDOUT,start_new_session=True)
    PROCESSES.append(parent)
    wait(lambda:(RT/'parent').is_socket())
    ENV['WAYLAND_DISPLAY']='parent'
    process=subprocess.Popen([ENV['MULTISEAT_HYPRLAND'],'-c',str(config)],env=ENV,stdout=open(BASE/'Hyprland.log','w'),stderr=subprocess.STDOUT,start_new_session=True)
    PROCESSES.append(process)
    ENV['HYPRLAND_INSTANCE_SIGNATURE']=wait(lambda:next((p.name for p in (RT/'hypr').glob('*') if (p/'.socket.sock').is_socket()),None))
    wait(lambda:ctl('monitors',True) is not None)
    monitors=ctl('monitors',True)
    ok('output create headless physical')
    ok('eval hl.monitor({output="physical",mode="1280x800",position="0x0",scale=1})')
    for monitor in monitors:ok('eval hl.monitor({output='+json.dumps(monitor['name'])+',disabled=true})')
    ENV['WAYLAND_DISPLAY']=wait(lambda:next((p.name for p in RT.glob('wayland-*') if p.is_socket()),None))
    ok('dispatch hl.dsp.focus({monitor="physical"})');ok('dispatch hl.dsp.focus({workspace="1"})')
def cleanup():
    for process in reversed(PROCESSES):
        if process.poll() is None:
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass
    for process in reversed(PROCESSES):
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL)
    print('Artifacts:',BASE,flush=True)

import uuid

OWNER = uuid.uuid4().hex + uuid.uuid4().hex
VIEW = uuid.uuid4().hex + uuid.uuid4().hex

def send(device, command):
    device.stdin.write(command + "\n"); device.stdin.flush()
    assert select.select([device.stdout], [], [], 5)[0] and device.stdout.readline().strip() == 'done', command

def key(device, code):
    for command in ('mods 64', 'key 125 1', f'key {code} 1', f'key {code} 0', 'key 125 0', 'mods 0'):
        send(device, command)

def state(): return ctl('seat state research', True)
def configure(overrides=True, owner=OWNER):
    value = {'owner':owner,'seatName':'research','seatId':state()['seatId'],'callback':str(RT/'controller.sock'),
        'bindings':[{'keys':['SUPER','2'],'action':'workspace','argument':'name:research-two','overrideInherited':overrides},
                    {'keys':['SUPER','2'],'action':'workspace','argument':'name:observer-only','viewOnly':True,'overrideInherited':overrides},
                    {'keys':['SUPER','A'],'action':'notify','argument':'inspect'},
                    {'keys':['SUPER','A'],'action':'notify','argument':'inspect','viewOnly':True}], 'overlays':[]}
    return ctl('seat configure ' + json.dumps(value))

def receive(device):
    key(device,30)
    connection,_=listener.accept()
    with connection:
        data=b''
        while b'\n' not in data: data+=connection.recv(4096)
    return json.loads(data)['params']

try:
    initialize()
    ok('seat create research physical'); ok('seat workspace research name:research-one')
    for source,name in (('virtual-keyboard-unstable-v1','virtual-keyboard'),('wlr-virtual-pointer-unstable-v1','virtual-pointer')):
        for mode,extension in (('client-header','h'),('private-code','c')):
            subprocess.run(['wayland-scanner',mode,str(FORK/'protocols'/(source+'.xml')),str(BASE/(name+'.'+extension))],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client','xkbcommon'],text=True).split()
    subprocess.run(['cc','-I'+str(BASE),str(FORK/'hyprtester/multiseat/input.c'),str(BASE/'virtual-keyboard.c'),str(BASE/'virtual-pointer.c'),'-o',str(BASE/'input'),*flags],check=True)
    def device(name,display):
        process=subprocess.Popen([str(BASE/'input'),name,'physical'],env=ENV|{'WAYLAND_DISPLAY':display},stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open(BASE/(name+'-input.log'),'w'),text=True,start_new_session=True)
        PROCESSES.append(process)
        assert select.select([process.stdout],[],[],5)[0] and process.stdout.readline().strip()=='ready'
        return process
    background=device('research',state()['display']); primary=device('Hyprland',ENV['WAYLAND_DISPLAY'])
    config=BASE/'hyprland.lua'
    config.write_text(config.read_text()+'\nhl.bind("SUPER + 2",hl.dsp.focus({workspace="name:inherited"}),{})\n')
    ok('eval hl.bind("SUPER + 2",hl.dsp.focus({workspace="name:inherited"}),{})')
    assert 'conflicts' in configure(False)
    answer=json.loads(configure()); assert answer['configured'] and 'SUPER + 2' in answer['inheritedOverrides'],answer
    assert 'another controller' in configure(owner='independent-controller-owner-00000000002')
    original=primary_state()
    key(background,3); assert state()['workspace']=='research-two',state()
    assert primary_state()==original
    key(primary,3); assert ctl('activeworkspace',True)['name']=='inherited'
    record('generic bindings override only the selected seat; existing primary shortcuts remain native without a shell')
    listener=socket.socket(socket.AF_UNIX);listener.settimeout(5);listener.bind(str(RT/'controller.sock'));listener.listen()
    event=receive(background); assert event['mode']=='control' and event['action']=='inspect'
    assert ctl('seat validate-context '+event['actionId']+' '+OWNER,True)['name']=='research'
    assert ctl('seat validate-context '+event['actionId']+' wrong-owner')=='stale action context'
    record('generic notifications are unicast to their controller and carry a validated action context')
    ok('seat ensure-workspace research '+state()['seatId']+' name:observer-only')
    assert ctl('seat present research '+state()['seatId']+' physical current '+VIEW,True)['active']
    background_before=state()['workspace']
    key(primary,3); view=ctl('seat presentation '+VIEW,True)
    assert view['workspace']=='observer-only' and state()['workspace']==background_before,(view,state())
    event=receive(primary);assert event['mode']=='readonly'
    assert ctl('seat validate-context '+event['actionId']+' '+OWNER,True)['mode']=='readonly'
    background_event=receive(background);assert background_event['mode']=='control'
    assert ctl('seat validate-context '+background_event['actionId']+' '+OWNER,True)['mode']=='control'
    assert ctl('seat present research '+state()['seatId']+' physical current '+VIEW,True)['active']
    assert 'changed' in ctl('seat validate-context '+event['actionId']+' '+OWNER)
    ok('seat unpresent '+VIEW)
    record('readonly view has independent workspace state; hidden-seat actions stay writable; changed view invalidates pending actions')
    command=shlex.join(['/bin/sh','-c','env > '+shlex.quote(str(BASE/'exec-env'))])
    ok('eval hl.bind("SUPER + T",hl.dsp.exec_cmd('+json.dumps(command)+'),{})')
    key(background,20)
    environment=wait(lambda:dict(line.split('=',1) for line in (BASE/'exec-env').read_text().splitlines() if '=' in line))
    context=environment['HYPRLAND_ACTION_ID']
    assert environment['HYPRLAND_SEAT_NAME']=='research'
    assert ctl('seat validate-context '+context,True)['seatId']==state()['seatId']
    ok('seat context-dispatch '+context+' hl.dsp.focus({workspace="name:async-target"})')
    assert state()['workspace']=='async-target'
    ctl('seat control research '+state()['seatId']+' '+state()['generation']+' pause',True)
    before=primary_state();assert ctl('seat context-dispatch '+context+' hl.dsp.focus({workspace="1"})')=='stale action context'
    assert primary_state()==before
    record('generic exec inherits seat context; revoked async actions fail without fallback to primary')
    current=state();ctl('seat control research '+current['seatId']+' '+current['generation']+' resume',True)
    # Recreate the virtual device after explicit generation revocation.
    background=device('research',state()['display'])
    assert json.loads(configure())['configured']
    ok('reload');assert ctl('seat configuration-renew '+OWNER)=='configuration registry was reloaded'
    assert json.loads(configure())['configured']
    key(background,3);assert state()['workspace']=='research-two'
    time.sleep(5.2);assert ctl('seat configuration-renew '+OWNER)=='configuration lease expired'
    key(background,3);assert state()['workspace']=='inherited'
    record('reload requires explicit configuration restoration; expired controllers stop overriding original bindings')
    current=state()
    ctl('seat lock-policy research '+current['seatId']+' '+current['generation']+' allow',True)
    ok('seat create-private-output isolated-private')
    wait(lambda:any(m['name']=='isolated-private' for m in ctl('monitors',True)))
    for mode,extension in (('client-header','h'),('private-code','c')):
        subprocess.run(['wayland-scanner',mode,str(FORK/'protocols/hyprland-lock-scope-v1.xml'),str(BASE/('lock-scope.'+extension))],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    subprocess.run(['cc','-I'+str(BASE),str(FORK/'hyprtester/multiseat/lock-scope.c'),str(BASE/'lock-scope.c'),'-o',str(BASE/'scope-probe'),*flags],check=True)
    locker=subprocess.Popen([str(BASE/'scope-probe')],env=ENV,stdin=subprocess.PIPE,stdout=open(BASE/'scope-events','w'),stderr=open(BASE/'scope.log','w'),text=True,start_new_session=True)
    PROCESSES.append(locker)
    wait(lambda:ctl('seat lock-state',True)['secure'])
    lockstate=ctl('seat lock-state',True)
    assert state()['paused'] and {o['name'] for o in lockstate['protectedOutputs']}=={'physical','isolated-private'},lockstate
    assert 'secure' in (BASE/'scope-events').read_text()
    locker.stdin.write('unlock\n');locker.stdin.flush();wait(lambda:not ctl('seat lock-state',True)['locked'])
    assert state()['paused']
    record('standalone scope controller protects every seat/output by default, even preauthorized seats and private outputs; unlock never resumes input')
finally:
    cleanup()
