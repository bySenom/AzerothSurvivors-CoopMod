"""Exercise the real native host over TCP. This is not multiplayer gameplay acceptance."""
from pathlib import Path
import argparse,json,socket,struct,subprocess,time,secrets

ROOT=Path(__file__).resolve().parents[1]
SHA=b'96e24dcd0e53fc13a6fdef5826aa46f77a29aa1aad4abd585adf04e94b2756d3'
TOKEN=b'0123456789abcdef0123456789abcdef'

def exact(connection,count):
    result=b''
    while len(result)<count:
        chunk=connection.recv(count-len(result))
        assert chunk,'Native bridge closed before the response was complete'
        result+=chunk
    return result

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--pid',type=int,required=True)
    ap.add_argument('--port',type=int,default=27120); args=ap.parse_args()
    launcher=ROOT/'build-v2/bin/RelWithDebInfo/AzerothCoop.exe'
    def rpc(*arguments):
        return json.loads(subprocess.check_output([str(launcher),*map(str,arguments)],text=True))
    def count_worlds(state):
        # Schema 1 used a provisional field name before the collection's role
        # was established from opening and closing native world streams.
        return state.get('world_sessions',state.get('roster_entries'))
    def connect(service,token=TOKEN):
        connection=socket.create_connection(('127.0.0.1',args.port),timeout=4)
        connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        connection.sendall(b'AZSC'+struct.pack('<HH',2,service)+SHA+token+secrets.token_bytes(16))
        response=exact(connection,8)
        assert response[:4]==b'AZSO',response.hex()
        return connection,response[4]
    initial=rpc('status',args.pid); initial_bridge=rpc('transport',args.pid,'status')
    assert initial['hook_installed'] and initial_bridge['listening']
    baseline=count_worlds(initial)
    bad,code=connect(8085,b'x'*32)
    assert code!=0,'Wrong room token was accepted'
    bad.close()
    channels=[]
    for service in (8085,8096):
        connection,code=connect(service); assert code==0
        reply=exact(connection,8)
        # Actual original server response: size BE=6, opcode LE=0x1ec,
        # native seed 0xc0ffee00. Verified against original emitted bytes.
        assert reply==bytes.fromhex('0006ec0100eeffc0'),reply.hex()
        channels.append(connection)
    concurrent=rpc('status',args.pid)
    assert count_worlds(concurrent)==baseline+2,(baseline,concurrent)
    auth,code=connect(3724); assert code==0
    account=b'SURVIVOR'
    # Original protocol shape from benilla-protocol/src/auth.rs:113-139.
    body=struct.pack('<I3BHIIII',0x00576f57,1,12,1,5875,0x00783836,0x0057696e,0x656e5553,0)
    body+=bytes([127,0,0,1,len(account)])+account
    request=b'\x00\x03'+struct.pack('<H',len(body))+body
    for part in [request[:1],request[1:3],request[3:11],request[11:]]:
        auth.sendall(part); time.sleep(0.01)
    prefix=exact(auth,35); assert prefix[:3]==b'\x00\x00\x00',prefix.hex()
    generator=exact(auth,exact(auth,1)[0]); assert len(generator)==1 and generator[0]>1
    prime=exact(auth,exact(auth,1)[0]); assert len(prime)==32
    salt=exact(auth,32); crc_salt=exact(auth,16); security=exact(auth,1)
    assert security==b'\x00'
    for connection in [auth,*channels]: connection.close()
    for _ in range(100):
        final=rpc('status',args.pid); bridge=rpc('transport',args.pid,'status')
        if count_worlds(final)==baseline and bridge['active_streams']==0: break
        time.sleep(0.025)
    assert count_worlds(final)==baseline,(baseline,final)
    assert bridge['active_streams']==0,bridge
    assert final['server_ticks']>initial['server_ticks']
    assert bridge['received_bytes']>initial_bridge['received_bytes']
    result=dict(real_native_tcp_bridge=True,wrong_room_token_rejected=True,concurrent_native_world_sessions=2,
                world_challenge_services=[8085,8096],fragmented_logon_challenge_roundtrip=True,
                auth_challenge_response_bytes=len(prefix)+1+len(generator)+1+len(prime)+len(salt)+len(crc_salt)+len(security),
                native_world_sessions_returned_to_baseline=True,server_ticks_before=initial['server_ticks'],
                server_ticks_after=final['server_ticks'],bridge=bridge,
                shared_simulation_verified=False,multiplayer_gameplay_verified=False)
    path=ROOT/'runtime/transport-integration-result.json'; path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))

if __name__=='__main__': main()
