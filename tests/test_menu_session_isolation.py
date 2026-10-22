"""Regression for run HUD/choice events leaking into the character picker.

Uses two real native world sessions plus an authenticated menu/preview session.
This validates protocol and native routing, not rendered UI acceptance.
"""
from pathlib import Path
import argparse,json,select,struct,time
from native_client import NativeClient,exact
from inspect_native_sessions import enter
from test_shared_frames import gameplay,move

ROOT=Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--pid',type=int,required=True);ap.add_argument('--port',type=int,required=True);args=ap.parse_args()
    result={'rendered_ui_verified':False}; controls=[]
    ui=[[],[],[]];runs=[[],[],[]];epochs=[0,0,0];completes=[0,0,0]
    def observe(clients,seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            ready,_,_=select.select([c.world for c in clients]+controls,[],[],0.03)
            for i,sock in enumerate(controls):
                if sock not in ready:continue
                length=struct.unpack('<I',exact(sock,4))[0];assert 9<=length<=2*1024*1024+9
                frame=exact(sock,length);kind=frame[0];epoch=struct.unpack_from('<Q',frame,1)[0]
                ui[i].append({'kind':kind,'epoch':epoch,'event':json.loads(frame[9:]) if kind==1 else None})
                if kind==2:epochs[i]=epoch
                elif kind==3:epochs[i]=0
                elif kind==1:
                    assert epochs[i]==epoch and epoch,'UI event outside its entered world'
                    event=ui[i][-1]['event']
                    if event.get('type')=='choices' and event.get('choices') and i<2:
                        clients[i].send(0x17c,struct.pack('<QI',guids[i],0))
            for i,client in enumerate(clients):
                if client.world not in ready:continue
                op,body=client.receive()
                if op==0x4d:completes[i]+=1
                if op==0x96 and b'AZSRUN' in body:
                    runs[i].extend(p.decode(errors='replace') for p in body.split(b'\0') if b'AZSRUN' in p)
    try:
        with NativeClient(args.port) as a,NativeClient(args.port) as b,NativeClient(args.port) as menu:
            clients=[a,b,menu];controls=[c.connect(37999) for c in clients]
            for c in clients:c.logon().open_world()
            # Listing characters and connecting a UI channel do not enter a run.
            menu.characters();observe(clients,0.2)
            assert not ui[2] and not runs[2]
            assert gameplay(args.pid,'status')['player_connections']==0
            guids=[enter(a),enter(b)]
            # Original reserved backdrop login while the authoritative run exists.
            menu.send(0x3d,struct.pack('<Q',0xffffff));observe(clients,0.3)
            move(a,-9390,-633);move(b,-9396,-627,0.0);observe(clients,4)
            active=gameplay(args.pid,'status');result['active_state']=active
            assert not ui[2], 'character-picker UI received a run event'
            assert not runs[2], 'character-picker world received the run HUD'
            assert all(runs[:2]),'entered player did not receive its HUD'
            assert epochs[0] and epochs[1] and epochs[0]!=epochs[1]
            assert any(e['kind']==1 for e in ui[0]),'native run UI event not exercised'
            assert active['player_positions'][0][:2]==[-9390,-633] and active['player_positions'][1][:2]==[-9396,-627]
            old_epoch=epochs[1];b.send(0x4b);observe(clients,0.7)
            assert completes[1] and epochs[1]==0,'logout did not end guest UI eligibility'
            boundary=len(ui[1]);run_boundary=len(runs[1]);observe(clients,0.7)
            assert len(ui[1])==boundary and len(runs[1])==run_boundary,'run updates continued after logout'
            b.send(0x3d,struct.pack('<Q',guids[1]));b.send(0x26a,struct.pack('<Q',guids[1]));observe(clients,0.5)
            assert epochs[1] and epochs[1]!=old_epoch,'re-enter reused a stale UI epoch'
            assert not ui[2] and not runs[2]
            result.update(menu_ui_events=0,menu_run_hud_messages=0,menu_preview_did_not_replace_run=True,
                independent_entered_heroes_verified=True,private_ui_session_binding_verified=True,
                logout_stopped_updates=True,reenter_uses_new_epoch=True,passed=True)
    finally:
        for c in controls:c.close()
        result['ui_frames']=ui;result['run_message_counts']=[len(r) for r in runs]
        path=ROOT/'runtime/menu-session-isolation-result.json';path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps({k:v for k,v in result.items() if k not in ('ui_frames','active_state')},indent=2))

if __name__=='__main__':main()
