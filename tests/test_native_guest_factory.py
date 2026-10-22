"""Real host + native guest detoured factory, for each exact build adapter.

This covers the guest-only hook missed by protocol-client tests. It does not
replace a rendered two-PC co-op run. Stops only children created by this test.
"""
import argparse, ctypes, json, os, struct, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOKEN = '0123456789abcdef0123456789abcdef'

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--legacy', type=Path); ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    all_results = {}
    for index, (adapter, game) in enumerate([('current', args.game)] + ([('legacy', args.legacy)] if args.legacy else [])):
        data = (args.output / adapter).resolve(); data.mkdir()
        binary = ROOT/'bin'/('legacy' if adapter == 'legacy' else '')
        launcher = binary/'AzerothCoop.exe'; host = None
        try:
            ps_env = {k: v for k, v in os.environ.items() if k.upper() != 'PSMODULEPATH'}
            text = subprocess.check_output(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(ROOT/'Launch-CoopMod.ps1'),
                '-Mode', 'Host', '-GamePath', str(game), '-DataRoot', str(data), '-Profile', 'Host', '-Address', '127.0.0.1',
                '-Port', str(27420+index), '-RoomToken', TOKEN, '-Minimized', '-NoErrorDialog'], text=True, encoding='utf-8', env=ps_env)
            host = json.loads(text)
            def rpc(*cmd):
                return json.loads(subprocess.check_output([str(launcher), *map(str, cmd)], text=True))
            for _ in range(100):
                state = rpc('status', host['pid'])
                if state['server_ticks'] > 3: break
                time.sleep(.1)
            assert state['hook_installed'] and state['layout_valid']
            session = data/'logs/sessions/guest-factory'; session.mkdir(parents=True)
            (session/'session.json').write_text(json.dumps(dict(schema=1, adapter=adapter, mode='native-guest-factory')))
            env = dict(os.environ, AZS_COOP_MODULE_CACHE=str(data/'module-cache'),
                AZS_COOP_WATCHER=str(ROOT/'bin/Azeroth Survivors Koop.exe'), AZS_COOP_CRASH_DIR=str(session))
            body = struct.pack('<I3BHIIII',0x00576f57,1,12,1,5875,0x00783836,0x0057696e,0x656e5553,0)
            body += bytes([127,0,0,1,8])+b'SURVIVOR'
            env['AZS_CLIENT_PROBE_AUTH_HEX'] = (b'\x00\x03'+struct.pack('<H',len(body))+body).hex()
            completed = subprocess.run([str(launcher), 'client-probe', str(game), str(data/'profiles/Guest'),
                str(binary/'azs_coop.dll'), str(binary/'azs_transport.dll'), '127.0.0.1', str(27420+index), TOKEN],
                env=env, capture_output=True, text=True, timeout=40)
            assert completed.returncode == 0, completed.stdout+completed.stderr
            replies = [json.loads(row) for row in completed.stdout.splitlines() if row.strip()]
            assert replies[0]['hex'] == '0006ec0100eeffc0' and replies[1]['bytes'] == 119
            for _ in range(100):
                if (session/'exit.json').exists(): break
                time.sleep(.05)
            exit_report = json.loads((session/'exit.json').read_text(encoding='utf-8-sig'))
            assert not exit_report['unexpected'], exit_report
            # The native launcher has returned; its independent watcher still
            # records host status and notices our later cleanup termination.
            time.sleep(2.5)
            host_session = Path(host['sessionPath'])
            status = json.loads((host_session/'status.json').read_text(encoding='utf-8-sig'))
            assert status['role'] == 'host' and status['samples']
            assert json.loads((host_session/'watcher.json').read_text(encoding='utf-8-sig'))['pid'] == host['pid']
            all_results[adapter] = dict(native_guest_factory_world_challenge=True, native_guest_factory_auth_bytes=119,
                watcher_survived_native_launcher_exit=True, guest_exit=exit_report, rendered_two_pc_acceptance=False)
        finally:
            if host:
                k32 = ctypes.WinDLL('kernel32', use_last_error=True)
                k32.OpenProcess.argtypes=[ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]; k32.OpenProcess.restype=ctypes.c_void_p
                k32.TerminateProcess.argtypes=[ctypes.c_void_p,ctypes.c_uint]; k32.CloseHandle.argtypes=[ctypes.c_void_p]
                handle=k32.OpenProcess(1,False,host['pid'])
                if handle: k32.TerminateProcess(handle,0); k32.CloseHandle(handle)
    all_results['passed'] = True
    (args.output/'result.json').write_text(json.dumps(all_results, indent=2))
    print(json.dumps(all_results, indent=2))

if __name__ == '__main__': main()
