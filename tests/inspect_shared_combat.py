"""Real native simulation test through two authenticated protocol clients, without UI input."""
from pathlib import Path
import argparse,collections,json,select,struct,time
from native_client import NativeClient,exact
from native_packets import Reader,objects,combat
from inspect_native_sessions import enter
from test_shared_frames import gameplay,move

ROOT=Path(__file__).resolve().parents[1]
RESULTS=ROOT/'runtime'

def main():
    RESULTS.mkdir(parents=True,exist_ok=True)
    ap=argparse.ArgumentParser();ap.add_argument('--pid',required=True,type=int);ap.add_argument('--port',required=True,type=int)
    ap.add_argument('--seconds',type=float,default=20);ap.add_argument('--auto-upgrades',action='store_true');args=ap.parse_args()
    reports=[dict(counts=collections.Counter(),creates=[],combat=[],moves=[],health={},runs=[]) for _ in range(2)]
    ui_connection=None; ui_events=[]; upgrade_requests=[]; last_choice=[0.0,0.0]
    def observe(clients,seconds):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            readable=[c.world for c in clients]+([ui_connection] if ui_connection else [])
            ready,_,_=select.select(readable,[],[],0.03)
            if ui_connection in ready:
                length=struct.unpack('<I',exact(ui_connection,4))[0];assert 9<=length<=2*1024*1024+9
                event=exact(ui_connection,length)
                if event[0] in (2,3):continue
                assert event[0]==1
                ui_events.append(event[9:].decode('utf-8'))
                if args.auto_upgrades:
                    data=json.loads(ui_events[-1])
                    if data.get('type')=='choices' and data.get('choices') and result.get('guids'):
                        b.send(0x17c,struct.pack('<QI',result['guids'][1],0))
                        upgrade_requests.append(dict(player=1,level=data.get('level'),choice=0))
            for n,c in enumerate(clients):
                if c.world not in ready:continue
                op,body=c.receive();out=reports[n];out['counts'][hex(op)]+=1
                if op in (0xa9,0x1f6):
                    for obj in objects(op,body):
                        if obj['kind'] in (2,3):out['creates'].append({k:v for k,v in obj.items() if k!='fields'})
                        if 'fields' in obj and 22 in obj['fields']:
                            out['health'].setdefault(str(obj['guid']),[]).append(obj['fields'][22])
                event=combat(op,body)
                if event and len(out['combat'])<2000:out['combat'].append(event)
                if op in (0xb5,0xb7,0xee) and len(out['moves'])<40:
                    r=Reader(body);guid=r.guid();flags,stamp,*pos=r.unpack('II4f')
                    out['moves'].append(dict(guid=guid,flags=flags,position=pos))
                if op==0x96 and b'AZSRUN' in body:
                    out['runs'].extend(p.decode(errors='replace') for p in body.split(b'\0') if b'AZSRUN' in p)
                    if args.auto_upgrades and n==0 and out['runs'] and 'ph=choosing;' in out['runs'][-1] and time.monotonic()-last_choice[0]>0.5:
                        c.send(0x17c,struct.pack('<QI',result['guids'][0],0));last_choice[0]=time.monotonic()
                        upgrade_requests.append(dict(player=0,choice=0))
    result={'visual_gameplay_verified':False,'complete_coop_verified':False}
    try:
        with NativeClient(args.port) as a,NativeClient(args.port) as b:
            ui_connection=b.connect(37999)
            a.logon().open_world();first=enter(a);observe([a],0.3)
            b.logon().open_world();second=enter(b);observe([a,b],0.5)
            result['guids']=[first,second];result['initial_state']=gameplay(args.pid,'status')
            move(a,-9390,-633);observe([a,b],0.4)
            move(b,-9396,-627,0.0);observe([a,b],args.seconds)
            result['final_state']=gameplay(args.pid,'snapshot')
            result['independent_positions_verified']=result['final_state']['player_positions'][0][:2]==[-9390,-633] and result['final_state']['player_positions'][1][:2]==[-9396,-627]
            result['distinct_character_guids_verified']=first!=second
            result['both_heroes_visible_in_packets']=all({first,second}<={o['guid'] for o in r['creates'] if o.get('object_type')==4} for r in reports)
            result['both_native_spell_casters_verified']=all({first,second}<={e['actor'] for e in r['combat'] if e['kind']=='spell'} for r in reports)
            result['both_movement_relays_verified']=first in {m['guid'] for m in reports[1]['moves']} and second in {m['guid'] for m in reports[0]['moves']}
            result['personal_run_hud_verified']=all(r['runs'] and marker in r['runs'][-1] for r,marker in zip(reports,['px=-9390.0;py=-633.0','px=-9396.0;py=-627.0']))
    finally:
        if ui_connection:ui_connection.close()
        result['native_ui_events']=ui_events
        result['upgrade_requests']=upgrade_requests
        result['clients']=reports
        (ROOT/'runtime/native-shared-combat-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        (ROOT/f'runtime/native-shared-combat-{args.pid}-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps({k:v for k,v in result.items() if k not in ('clients','initial_state','final_state','native_ui_events')},indent=2))
        for event in ui_events[:4]:print('native guest UI event',event[:700])
        for field in ('last_host_event_hex','last_guest_event_hex'):
            if result.get('final_state',{}).get(field):print(field,bytes.fromhex(result['final_state'][field]).decode(errors='replace')[:700])
        for n,r in enumerate(reports):
            print('client',n,'packets',sum(r['counts'].values()),'creates',len(r['creates']),'combat',len(r['combat']))
            print('hero health ranges', {g:[min(r['health'][str(g)]),max(r['health'][str(g)]),r['health'][str(g)][-1]] for g in result.get('guids',[]) if str(g) in r['health']})
            print('last run HUD',r['runs'][-1:])
    assert result.get('independent_positions_verified')
    assert result.get('distinct_character_guids_verified')
    assert result.get('both_native_spell_casters_verified')
    assert result.get('both_movement_relays_verified')
    assert result.get('both_heroes_visible_in_packets')
    assert result.get('personal_run_hud_verified')

if __name__=='__main__':main()
