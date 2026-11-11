#!/usr/bin/env python3
# In-game bisect of native vs oracle (RG_NATIVE_CPU_ONLY, src/native_cpu.cpp):
# which native function keeps the boot walk from reaching the field
# ([overlay] load #2). Logs in build/run/gbisect. Found SaveModeDispatch
# (0x375df0) in 15 boots, 2026-11-08.
import os, subprocess, sys, time, signal
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../..'))
S = os.path.join(ROOT, 'build/run/gbisect'); os.makedirs(S, exist_ok=True)
os.chdir(ROOT)
addrs = sorted(int(l.split()[0], 16) for l in open('build/generated/native/funcs.txt') if l.strip() and not l.startswith('#'))
env = dict(os.environ)
env.update(RG_NATIVE_CPU='1', RG_VU1_NATIVE='gpu',
    RG_INPUT="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5",
    RG_INPUT_CLOCK='pad', RG_MUTE='1', DQ8_GFX_HIDDEN_WINDOW='1', RG_FPS='60', RG_SCALE='1',
    RG_MC0_DIR='build/run/mc0-bench', RG_SETTINGS_FILE='build/run/bench-settings.ini', RG_STALL_WATCH='0')
env.setdefault('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')
def run(lo, hi, tag, limit=80):
    env['RG_NATIVE_CPU_ONLY'] = f'{lo:x}-{hi:x}'
    log = f'{S}/b_{tag}.log'
    with open(log, 'w') as out:
        p = subprocess.Popen(['build/game/src/roguegalaxy', 'disc/SCUS_974.90'], env=env, stdout=out, stderr=subprocess.STDOUT)
    t0 = time.time(); ok = False
    while time.time() - t0 < limit and p.poll() is None:
        time.sleep(2)
        if 'load #2' in open(log, errors='replace').read():
            ok = True; break
    p.send_signal(signal.SIGINT)
    try: p.wait(5)
    except subprocess.TimeoutExpired: p.kill(); p.wait()
    n = sum(lo <= a < hi for a in addrs)
    print(f'{tag}: native {lo:#x}-{hi:#x} ({n} fns) -> {"PASS" if ok else "FAIL"} in {time.time()-t0:.0f}s', flush=True)
    return ok
if subprocess.run(['pgrep', '-x', 'roguegalaxy']).returncode == 0:
    sys.exit('a game is running')
if not run(0, 0, 'none'):
    sys.exit('fails with nothing native: the runtime side (in-place waits), not a function')
lo, hi = addrs[0], addrs[-1] + 1
step = 0
while True:
    fns = [a for a in addrs if lo <= a < hi]
    if len(fns) == 1:
        print(f'CULPRIT: {lo:#x}'); break
    mid = fns[len(fns) // 2]
    step += 1
    if not run(lo, mid, f'{step}a'):
        hi = mid; continue
    if not run(mid, hi, f'{step}b'):
        lo = mid; continue
    print(f'neither half fails alone: interaction between {lo:#x}-{mid:#x} and {mid:#x}-{hi:#x}'); break
