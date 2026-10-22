"""Integration check for native frame replication; independent heroes are NOT asserted."""
from pathlib import Path
import argparse, collections, json, select, struct, subprocess, time
from native_client import NativeClient,GAME_OFFSETS
from inspect_native_sessions import enter

ROOT=Path(__file__).resolve().parents[1]

def gameplay(pid, *args):
    result=subprocess.run([str(ROOT/'build-v2/bin/RelWithDebInfo/AzerothCoop.exe'),
                           'gameplay',str(pid),*args],capture_output=True,text=True,check=True)
    return json.loads(result.stdout)

def observe(clients, seconds):
    until=time.monotonic()+seconds
    result=[dict(counts=collections.Counter(),runs=[]) for _ in clients]
    while time.monotonic()<until:
        ready,_,_=select.select([c.world for c in clients],[],[],0.05)
        for i,client in enumerate(clients):
            if client.world not in ready: continue
            op,body=client.receive(); result[i]['counts'][f'0x{op:04x}']+=1
            if op==0x96 and b'AZSRUN' in body:
                result[i]['runs'].extend(p.decode('utf-8',errors='replace') for p in body.split(b'\0') if b'AZSRUN' in p)
    return result

def move(client,x,y,orientation=1.6):
    # Original engine messages/movement.rs: flags,time,x/y/z/facing,fall_time.
    client.send(0xb5,struct.pack('<II4fI',1,int(time.monotonic()*1000)&0xffffffff,x,y,68.02375,orientation,0))

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--pid',type=int,required=True);ap.add_argument('--port',type=int,default=27121)
    args=ap.parse_args()
    result={'independent_heroes_verified':False}
    with NativeClient(args.port) as a, NativeClient(args.port) as b:
        a.logon().open_world();enter(a);observe([a],0.25)
        b.logon().open_world();enter(b);result['after_login']=observe([a,b],0.4)
        result['before']=gameplay(args.pid,'snapshot')
        move(a,-9390,-633);result['first_movement']=observe([a,b],0.6)
        result['after_first']=gameplay(args.pid,'snapshot')
        move(b,-9396,-627,0.0);result['second_movement']=observe([a,b],0.6)
        result['after_second']=gameplay(args.pid,'snapshot')
        # Every replicated native frame is reserialized by each session's cipher.
        assert result['after_second']['mirrored_packets']>result['before']['mirrored_packets']
        assert all(r['counts'] for r in result['first_movement']), 'one session received no movement/frame data'
        result['native_frame_replication_verified']=True
    out=ROOT/'runtime/native-frame-replication-result.json'
    out.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('before','after_first','after_second')},indent=2))
    for key in ('before','after_first','after_second'):
        data=bytes.fromhex(result[key]['game_bytes'])
        print(key,'native player position',struct.unpack_from('<4f',data,GAME_OFFSETS['position']),
              'native run seconds',struct.unpack_from('<f',data,GAME_OFFSETS['clock'])[0],'native phase byte',data[GAME_OFFSETS['phase']])

if __name__=='__main__':main()
