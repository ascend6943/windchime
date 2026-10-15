import os, socket, subprocess, time, pathlib, tempfile

root=pathlib.Path(__file__).resolve().parent.parent
if os.geteuid()==0:
    raise SystemExit('SKIP: run helper protocol checks as an unprivileged user.')
if not pathlib.Path('/run/dbus/system_bus_socket').exists():
    raise SystemExit('SKIP: helper protocol checks require a system bus.')
if any(os.access(p,os.R_OK) for p in pathlib.Path('/dev/input').glob('event*')):
    raise SystemExit('SKIP: this user can read input devices; refusing a physical-input test.')
temporary=tempfile.TemporaryDirectory(prefix='windchime-protocol-')
sockpath=pathlib.Path(temporary.name)/'helper.sock'
server=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
server.bind(str(sockpath));server.listen(4)
fd=server.fileno()
launch='''import os,sys
fd=int(sys.argv[1]);os.dup2(fd,3);os.set_inheritable(3,True)
os.environ['LISTEN_PID']=str(os.getpid());os.environ['LISTEN_FDS']='1'
os.execv(sys.argv[2],[sys.argv[2],str(os.getuid())])
'''
p=subprocess.Popen(['python3','-c',launch,str(fd),str(root/'build/windchime-input')],pass_fds=(fd,),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
server.close()
def read_until(s,wanted,seconds=4):
    end=time.monotonic()+seconds;data=b''
    while time.monotonic()<end:
        s.settimeout(max(.01,end-time.monotonic()))
        try:data+=s.recv(4096)
        except socket.timeout:break
        if wanted in data:return data
    raise AssertionError((wanted,data))
try:
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.connect(str(sockpath))
    assert b'S paused 0\n' in read_until(s,b'S paused 0\n')
    # A second client cannot acquire this UID's active connection.
    other=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);other.connect(str(sockpath));other.settimeout(2);assert other.recv(1)==b'';other.close()
    s.sendall(b'C 5 1\n')
    s.settimeout(2);state=s.recv(4096)
    assert b'S locked 0\n' in state or b'S no-keyboard 0\n' in state, state
    # The unprivileged test process never has device access. No note can appear.
    assert b'N ' not in state
    s.sendall(b'C 0 1\n')
    assert b'S paused 0\n' in read_until(s,b'S paused 0\n')
    s.sendall(b'C 10 1\n');read_until(s,b'S ')
    # Missing heartbeat disables listening, even if the client remains open.
    if b'locked' not in state:assert b'S paused 0\n' in read_until(s,b'S paused 0\n',5)
    s.close();time.sleep(.2)
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.connect(str(sockpath));read_until(s,b'S paused 0\n')
    s.sendall(b'X'*600);s.settimeout(2)
    try:closed=s.recv(4096)
    except ConnectionResetError:closed=b''
    assert closed==b''
    print('PASS: socket activation, paused default, one client, invalid settings, heartbeat expiry, reconnect and oversized-message rejection.')
finally:
    p.terminate()
    try:p.wait(timeout=3)
    except subprocess.TimeoutExpired:p.kill();p.wait()
    stderr=p.stderr.read().decode()
    if stderr:print(stderr)
    sockpath.unlink(missing_ok=True)
    temporary.cleanup()
