"""Measure original world-login/update behavior on a disposable native host."""
from pathlib import Path
import argparse,collections,json,select,struct,time
from native_client import NativeClient

ROOT=Path(__file__).resolve().parents[1]
def enter(client):
    roster=client.characters()
    assert roster and roster[0]>0
    guid=struct.unpack_from('<Q',roster,1)[0]
    client.send(0x3d,struct.pack('<Q',guid))
    client.send(0x26a,struct.pack('<Q',guid))
    return guid
def observe(client,seconds):
    until=time.monotonic()+seconds; counts=collections.Counter(); runs=[]; locations=[]
    while time.monotonic()<until:
        if not select.select([client.world],[],[],0.05)[0]: continue
        opcode,body=client.receive(); counts[f'0x{opcode:04x}']+=1
        if opcode==0x236 and len(body)>=20:
            locations.append(struct.unpack_from('<I4f',body))
        if opcode==0x96 and b'AZSRUN' in body:
            for part in body.split(b'\0'):
                if b'ph=' in part or b't=' in part: runs.append(part.decode('utf-8',errors='replace'))
    return dict(packet_counts=dict(counts),run_messages=runs[:12],locations=locations[:3])
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--port',type=int,default=27121); args=ap.parse_args()
    with NativeClient(args.port) as a, NativeClient(args.port) as b:
        a.logon().open_world(); first_guid=enter(a); first=observe(a,0.5)
        b.logon().open_world(); second_guid=enter(b); second=observe(b,0.5)
        later_a=observe(a,0.5); later_b=observe(b,0.5)
        result=dict(first_guid=first_guid,second_guid=second_guid,first_login=first,second_login=second,
                    first_later=later_a,second_later=later_b,shared_multiplayer_heroes_verified=False)
    (ROOT/'runtime/native-session-behavior.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))
if __name__=='__main__': main()
