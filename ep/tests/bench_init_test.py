"""Exercise distributed init order and absence of shared-file sweeps."""
import ast,os,sys,tempfile
from datetime import timedelta
from pathlib import Path
from types import SimpleNamespace
from typing import Optional

cleanup_only='--cleanup-only' in sys.argv
args=[arg for arg in sys.argv[1:] if arg!='--cleanup-only']
source=Path(args[0]) if args else Path(__file__).with_name('utils.py')
module=ast.parse(source.read_text())
functions=[n for n in module.body if isinstance(n,ast.FunctionDef) and n.name in
           ('init_dist_under_torchrun','initialize_uccl','destroy_uccl')]
assert len(functions)==3
code=compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec')
class FakeCuda:
 def __init__(self,events):self.events=events
 def set_device(self,n):self.events.append(('set_device',n))
 def current_device(self):return 0
class FakeDist:
 def __init__(self,events):self.events=events
 def init_process_group(self,**kw):self.events.append(('init',kw))
 def get_rank(self):return 0
 def get_world_size(self):return 2
 def new_group(self,ranks):self.events.append(('group',ranks));return 'group'
 def barrier(self,group):self.events.append(('barrier',group))
class FakeEp:
 def __init__(self,events):self.events=events
 def get_num_proxy_threads(self):return 0
 def register_proxies(self,dev,proxies):self.events.append(('register',dev))
 def unregister_proxy(self,dev):self.events.append(('unregister',dev))
class FakeOs:
 def __init__(self,env,events):self.environ=env;self.events=events
 def getenv(self,name,default=None):return self.environ.get(name,default)
 def remove(self,path):self.events.append(('remove',path))
class NoSweepGlob:
 def __init__(self,events):self.events=events
 def glob(self,path):self.events.append(('glob',path));return ['/tmp/unrelated-barrier']

def build(env):
 events=[];cuda=FakeCuda(events);dist=FakeDist(events);ep=FakeEp(events)
 torch=SimpleNamespace(cuda=cuda,bfloat16='bf16',device=lambda x:x,
  set_default_dtype=lambda x:events.append(('dtype',x)),
  set_default_device=lambda x:events.append(('default_device',x)))
 g={'os':FakeOs(env,events),'glob':NoSweepGlob(events),'torch':torch,'dist':dist,'ep':ep,'Optional':Optional,
    'timedelta':timedelta,'time':SimpleNamespace(sleep=lambda t:None),
    'detect_group_topology':lambda group:(0,0,0,0,2,False),
    'get_cpu_proxies_meta':lambda *args:{0:{},1:{}}}
 exec(code,g);return g,events
if not cleanup_only:
 for value in (None,'0','1'):
  env={} if value is None else {'UCCL_BENCH_LAZY_INIT':value}
  g,events=build(env);assert g['init_dist_under_torchrun'](2,1)==(0,2,'group')
  assert events[0]==('set_device',2) and events[1][0]=='init'
  if value=='1':assert events[1][1]=={'backend':'nccl','timeout':timedelta(seconds=100)}
  else:assert events[1][1]=={'backend':'nccl','device_id':'cuda:2'}
  assert events[-1]==('group',[0,1])
 g,events=build({'UCCL_BENCH_LAZY_INIT':'invalid'})
 try:g['init_dist_under_torchrun'](0,1)
 except ValueError:assert not events
 else:raise AssertionError('invalid lazy mode accepted')
g,events=build({'LOCAL_RANK':'0'})
proxies,workers=g['initialize_uccl'](1,64,0,2,'group',use_normal_mode=True)
assert proxies==[] and workers is None
g['destroy_uccl'](proxies,workers)
assert ('register',0) in events and ('unregister',0) in events
assert not any(event[0] in ('glob','remove') for event in events),events
print('BENCH_SAFETY_TESTS_PASSED init-order/lazy/eager/no-shared-sweep')
