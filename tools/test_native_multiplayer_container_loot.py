#!/usr/bin/env python3
"""Test host or guest approach from five tiles, Bones loot UI, locks, and following combat.

Requires xvfb-run, xdotool, and ImageMagick import. Artifacts use virtual displays.
"""
import argparse
import os,pathlib,shutil,subprocess,time,re,signal,tempfile,json
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=pathlib.Path)
parser.add_argument("data_root", type=pathlib.Path)
parser.add_argument("--port", type=int, default=49276)
parser.add_argument("--invalidate-loot", action="store_true", help="leave loot open and verify host combat placement closes it")
parser.add_argument("--debug-guest", action="store_true", help="run the guest under gdb and retain its backtrace")
parser.add_argument("--combat-loot", action="store_true", help="guest opens and takes all during its actual combat turn")
parser.add_argument("--host-looter", action="store_true", help="host Take All must empty both original Bones stacks")
options = parser.parse_args()
if options.combat_loot and (options.host_looter or options.invalidate_loot):
 parser.error("--combat-loot requires the guest without invalidation")
if options.host_looter and options.invalidate_loot:
 parser.error("--invalidate-loot requires the guest loot window")
looter_role = "host" if options.host_looter else "guest"
root=pathlib.Path(tempfile.mkdtemp(prefix='fallout-native-container-loot-', dir='/var/tmp'));print(root,flush=True)
base=options.data_root.resolve();binary=options.binary.resolve();procs=[]
env=dict(os.environ,SDL_AUDIODRIVER='dummy',SDL_RENDER_DRIVER='software')
def wait(fn,t=40):
 end=time.monotonic()+t
 while time.monotonic()<end:
  if fn():return
  time.sleep(.1)
 raise RuntimeError('timeout')
def log(role):return (root/role/'game.log').read_text()
def knife_count():
 for line in reversed((root/looter_role/'journal.jsonl').read_text().splitlines()):
  state=json.loads(line)
  if state.get('event')=='world_state':
   return sum(item['quantity'] for item in state.get('local_inventory',[]) if item['pid']==4)
 return 0
def x(role,*args):
 d=root/role;e=dict(env,DISPLAY=(d/'display').read_text(),XAUTHORITY=(d/'xauthority').read_text())
 subprocess.run(['xdotool',*map(str,args)],env=e,check=True)
try:
 for n,role in enumerate(['host','guest']):
  d=root/role;d.mkdir();shutil.copy2(binary,d/'fallout-ce');shutil.copytree((base/'DATA' if (base/'DATA').is_dir() else base/'data'),d/'data')
  config=(base/'fallout.cfg').read_text().replace('enabled=1','enabled=0')
  config=re.sub(r'(?m)^master_patches\s*=.*$', 'master_patches=data', config)
  config=re.sub(r'(?m)^critter_patches\s*=.*$', 'critter_patches=data', config)
  config=re.sub(r'(?m)^master_dat\s*=.*$', 'master_dat=master.dat', config)
  config=re.sub(r'(?m)^critter_dat\s*=.*$', 'critter_dat=critter.dat', config)
  (d/'fallout.cfg').write_text(config)
  for f in ['master.dat','critter.dat']:(d/f).symlink_to((base/f if (base/f).exists() else base/f.upper()).resolve())
  args=['./fallout-ce',(f'--multiplayer-host={options.port}' if n==0 else f'--multiplayer-join=127.0.0.1:{options.port}'),'--multiplayer-smoke-test','--multiplayer-smoke-scenario=combat-attack','--multiplayer-smoke-weapon=pistol',('--multiplayer-smoke-native-container-loot-host' if options.host_looter else '--multiplayer-smoke-native-container-loot'),f'--agent-journal={d}/journal.jsonl',f'--agent-command-file={d}/commands']
  if options.combat_loot:
   args = [a for a in args if not a.startswith('--multiplayer-smoke-weapon=') and not a.startswith('--multiplayer-smoke-native-container-loot')]
   args.append('--multiplayer-smoke-native-combat-loot')
  if options.debug_guest and role=='guest':
   args=['gdb','-batch','-ex','run','-ex','thread apply all bt','--args',*args]
  p=subprocess.Popen(['timeout','120s','xvfb-run','-a','--server-args=-screen 0 640x480x24','sh','-c','printf %s "$DISPLAY" > display; printf %s "$XAUTHORITY" > xauthority; exec "$@"','sh',*args],cwd=d,env=env,stdout=(d/'game.log').open('w'),stderr=subprocess.STDOUT,start_new_session=True);procs.append(p)
  if n==0:wait(lambda:'WAITING ON PORT' in log(role))
 if options.combat_loot:
  wait(lambda:'NATIVE_COMBAT_LOOT_READY role=guest' in log('guest'))
  before_ap=int(re.search(r'NATIVE_COMBAT_LOOT_READY role=guest ap=(\d+)',log('guest')).group(1))
  def combat_opened():
   rows=[json.loads(l) for l in (root/'guest'/'journal.jsonl').read_text().splitlines()]
   latest=next((r for r in reversed(rows) if r.get('event')=='world_state'),{})
   return latest.get('phase')=='combat' and latest.get('guest',{}).get('ap')==before_ap-3
  wait(combat_opened,10)
  time.sleep(1)
  x('guest','key','shift+a');time.sleep(1);x('guest','key','Escape')
  for role,p in zip(['host','guest'],procs):
   rc=p.wait()
   if rc!=0 or 'MULTIPLAYER_SMOKE_TEST_PASS' not in log(role) or 'SIGSEGV' in log(role):raise RuntimeError('combat loot failed '+role)
  if 'NATIVE_COMBAT_LOOT_PASS' not in log('guest'):raise RuntimeError('combat loot AP or transfer failed')
  digests=[re.search(r'digest=(\d+)',log(role)).group(1) for role in ['host','guest']]
  if digests[0]!=digests[1]:raise RuntimeError('combat loot final state differs')
  print('NATIVE_COMBAT_LOOT_TEST_PASS',root,flush=True)
  raise SystemExit(0)
 wait(lambda:f'NATIVE_CONTAINER_LOOT_READY role={looter_role}' in log(looter_role))
 time.sleep(3)
 d=root/looter_role;e=dict(env,DISPLAY=(d/'display').read_text(),XAUTHORITY=(d/'xauthority').read_text())
 subprocess.run(['import','-window','root',str(d/'loot.png')],env=e,check=True)
 print('UI captured',flush=True)
 if not options.host_looter:
  # Look at the target-side weapon, then drag it to the player inventory.
  before_knives=knife_count()
  def point(seq,px,py):
   with (d/'commands').open('a') as f:f.write(f'{seq} move {px} {py}\n')
   time.sleep(.2)
  point(1,532,60)
  x(looter_role,'mousedown',3);time.sleep(.2);x(looter_role,'mouseup',3);time.sleep(.3)
  x(looter_role,'mousedown',1);time.sleep(.2);x(looter_role,'mouseup',1);time.sleep(.5)
  subprocess.run(['import','-window','root',str(d/'look.png')],env=e,check=True)
  x(looter_role,'mousedown',3);time.sleep(.2);x(looter_role,'mouseup',3);time.sleep(.3)
  point(2,532,60);x(looter_role,'mousedown',1);time.sleep(.3)
  point(3,160,200);x(looter_role,'mouseup',1)
  wait(lambda:knife_count()==before_knives+1,10)
  subprocess.run(['import','-window','root',str(d/'drag.png')],env=e,check=True)
 x(looter_role,'key','shift+a')
 if options.invalidate_loot:
  wait(lambda: all('NATIVE_CONTAINER_LOOT_PASS' in log(role) for role in ['host','guest']),25)
  print('NATIVE_LOOT_INVALIDATED_WINDOW_CLOSED_PASS',root,flush=True)
  raise SystemExit(0)
 time.sleep(2);x(looter_role,'key','Escape')
 for role,p in zip(['host','guest'],procs):
  rc=p.wait();print(role,rc,flush=True)
  print('\n'.join(l for l in log(role).splitlines() if 'NATIVE_CONTAINER' in l or 'SMOKE_TEST' in l or 'rejected' in l),flush=True)
  if role=='host' and 'NATIVE_CONTAINER_LOCKED_REJECT_PASS' not in log(role):raise RuntimeError('locked container was not rejected')
  if rc!=0 or 'NATIVE_CONTAINER_LOOT_PASS' not in log(role):raise RuntimeError('native loot failure '+role)
 digests=[re.search(r'digest=(\d+)',log(role)).group(1) for role in ['host','guest']]
 if digests[0]!=digests[1]:raise RuntimeError('final state differs')
 print('NATIVE_CONTAINER_LOOT_TEST_PASS',root,flush=True)
finally:
 for p in procs:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=5)
