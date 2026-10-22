"""Protocol harness for the original local game backend, over our loopback bridge.

Contracts and SRP math are derived from the pinned reference-source copies of
benilla-protocol/src/{auth.rs,world/session.rs,messages/client.rs} and
benilla-srp/src/{lib.rs,vanilla_header.rs}. This is a test client, not a game
replacement or multiplayer gameplay acceptance. Only the fan game's fixed local
SURVIVOR test identity is used. No user account/session material is logged.
"""
from pathlib import Path
import hashlib,json,os,secrets,socket,struct,zlib

ROOT=Path(__file__).resolve().parents[1]
BUILD_SHA=os.environ.get('AZS_TEST_GAME_SHA','dc10453398ceb398076f6d3b6ac7859470de2ff7ccd68ac1d00a02f9f1e5209a').encode('ascii')
assert len(BUILD_SHA)==64 and all(c in b'0123456789abcdef' for c in BUILD_SHA)
GAME_OFFSETS = ({'position':0x490,'clock':0x994,'phase':0xa1b} if BUILD_SHA.startswith(b'96e24dcd') else
                {'position':0x4d0,'clock':0xa4c,'phase':0xadf})
USER=b'SURVIVOR'
PASSWORD=b'SURVIVOR'
def h(*parts): return hashlib.sha1(b''.join(parts)).digest()
def number(data): return int.from_bytes(data,'little')
def exact(connection,count):
    result=b''
    while len(result)<count:
        chunk=connection.recv(count-len(result))
        if not chunk: raise EOFError('Original native stream closed')
        result+=chunk
    return result

class HeaderCipher:
    def __init__(self,key): self.key=key; self.index=0; self.previous=0
    def encrypt(self,data):
        output=bytearray()
        for value in data:
            value=((value^self.key[self.index])+self.previous)&255
            output.append(value); self.previous=value; self.index=(self.index+1)%40
        return bytes(output)
    def decrypt(self,data):
        output=bytearray()
        for value in data:
            output.append(((value-self.previous)&255)^self.key[self.index])
            self.previous=value; self.index=(self.index+1)%40
        return bytes(output)

class NativeClient:
    def __init__(self,port,token='0123456789abcdef0123456789abcdef'):
        self.bridge_port=port; self.token=token.encode('ascii'); self.auth=None; self.world=None
        self.identity=secrets.token_bytes(16)
        self.key=None; self.send_cipher=None; self.receive_cipher=None
        self.server_proof_verified=False; self.realm_count=0
    def connect(self,service):
        connection=socket.create_connection(('127.0.0.1',self.bridge_port),timeout=4)
        connection.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        connection.sendall(b'AZSC'+struct.pack('<HH',2,service)+BUILD_SHA+self.token+self.identity)
        response=exact(connection,8)
        if response[:4]!=b'AZSO' or response[4]:
            connection.close(); raise RuntimeError('Native bridge refused the connection')
        return connection
    def logon(self):
        self.auth=self.connect(3724)
        body=struct.pack('<I3BHIIII',0x00576f57,1,12,1,5875,0x00783836,0x0057696e,0x656e5553,0)
        body+=bytes([127,0,0,1,len(USER)])+USER
        self.auth.sendall(b'\x00\x03'+struct.pack('<H',len(body))+body)
        prefix=exact(self.auth,3)
        if prefix!=b'\x00\x00\x00': raise RuntimeError('Native realm challenge refused: '+prefix.hex())
        B=exact(self.auth,32); g=number(exact(self.auth,exact(self.auth,1)[0]))
        prime=exact(self.auth,exact(self.auth,1)[0]); salt=exact(self.auth,32)
        crc=exact(self.auth,16); flags=exact(self.auth,1)
        assert len(prime)==32 and flags==b'\0'
        N=number(prime); x=number(h(salt,h(USER,b':',PASSWORD)))
        xor=bytes(a^b for a,b in zip(h(prime),h(bytes([g]))))
        for _ in range(512):
            a=number(secrets.token_bytes(32)); A=pow(g,a,N).to_bytes(32,'little')
            u=number(h(A,B)); S=pow((number(B)-3*pow(g,x,N))%N,a+u*x,N).to_bytes(32,'little')
            even=h(S[::2]); odd=h(S[1::2]); key=bytes(v for pair in zip(even,odd) for v in pair)
            proof=h(xor,h(USER),salt,A,B,key)
            if A[-1] and S[0] and key[-1] and proof[-1]: break
        else: raise RuntimeError('Could not create an unambiguous SRP test draw')
        integrity=bytes.fromhex('95edb27c7823b363cbddab56a392e7cb73fcca20')
        version=h(A,integrity) if crc==bytes.fromhex('baa31e99a00b2157fc373fb369cdd2f1') else bytes(20)
        self.auth.sendall(b'\x01'+A+proof+version+b'\0\0')
        reply=exact(self.auth,2)
        if reply!=b'\x01\x00': raise RuntimeError('Native realm proof refused: '+reply.hex())
        server_proof=exact(self.auth,20); exact(self.auth,4)
        if server_proof!=h(A,proof,key): raise RuntimeError('Native SRP server proof differs')
        self.server_proof_verified=True; self.key=key
        self.auth.sendall(b'\x10'+bytes(4))
        header=exact(self.auth,3); assert header[0]==0x10
        realms=exact(self.auth,int.from_bytes(header[1:],'little'))
        self.realm_count=realms[4]
        return self
    def open_world(self,service=8085):
        assert self.key and self.server_proof_verified
        self.world=self.connect(service)
        size,opcode=self._clear_header(exact(self.world,4)); challenge=exact(self.world,size-2)
        assert opcode==0x1ec and len(challenge)==4
        seed=secrets.token_bytes(4)
        proof=h(USER,bytes(4),seed,challenge,self.key)
        fixture=Path(__file__).parent/'fixtures/stock-addon-names.json'
        names=json.loads(fixture.read_text(encoding='utf-8-sig'))['names']
        assert len(names)==12
        addons=b''.join(name.encode()+b'\0\x01'+struct.pack('<II',0x4c1c776d,0) for name in names)
        body=struct.pack('<II',5875,0)+USER+b'\0'+seed+proof+struct.pack('<I',len(addons))+zlib.compress(addons)
        self.world.sendall(struct.pack('>H',len(body)+4)+struct.pack('<I',0x1ed)+body)
        self.send_cipher=HeaderCipher(self.key); self.receive_cipher=HeaderCipher(self.key)
        for _ in range(20):
            opcode,body=self.receive()
            if opcode==0x1ee:
                if not body or body[0]!=0x0c: raise RuntimeError('Native world authentication refused')
                return self
        raise RuntimeError('Native world authentication response missing')
    @staticmethod
    def _clear_header(header): return int.from_bytes(header[:2],'big'),int.from_bytes(header[2:],'little')
    def send(self,opcode,body=b''):
        header=struct.pack('>H',len(body)+4)+struct.pack('<I',opcode)
        self.world.sendall(self.send_cipher.encrypt(header)+body)
    def receive(self):
        header=self.receive_cipher.decrypt(exact(self.world,4)); size,opcode=self._clear_header(header)
        if not 2<=size<=65535: raise RuntimeError('Native world header did not decrypt')
        return opcode,exact(self.world,size-2)
    def characters(self):
        self.send(0x37)
        for _ in range(30):
            opcode,body=self.receive()
            if opcode==0x3b: return body
        raise RuntimeError('Native character enumeration missing')
    def close(self):
        for connection in (self.auth,self.world):
            if connection:
                try: connection.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                connection.close()
        self.auth=self.world=None
    def __enter__(self): return self
    def __exit__(self,*_): self.close()

if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser(); ap.add_argument('--port',type=int,required=True); args=ap.parse_args()
    with NativeClient(args.port) as client:
        client.logon().open_world(); roster=client.characters()
        result=dict(srp_server_proof_verified=client.server_proof_verified,realm_count=client.realm_count,
                    world_authenticated=True,character_count=roster[0] if roster else 0,character_enum_bytes=len(roster),
                    multiplayer_gameplay_verified=False)
        (ROOT/'runtime/native-authenticated-session-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps(result,indent=2))
