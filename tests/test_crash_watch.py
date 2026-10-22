"""Real process-exit/dump tests using disposable fixtures, no game or user input."""
import argparse, ctypes, json, struct, subprocess, time, zipfile
from ctypes import wintypes
from pathlib import Path

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateEventW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
k32.CreateEventW.restype = wintypes.HANDLE
k32.OpenEventW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
k32.OpenEventW.restype = wintypes.HANDLE
k32.SetEvent.argtypes = [wintypes.HANDLE]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    result = {}
    args.output.mkdir(parents=True, exist_ok=True)
    for mode in ('crash', 'normal', 'kill', 'failfast'):
        session = args.output / 'logs' / 'sessions' / mode
        session.mkdir(parents=True)
        (session/'session.json').write_text(json.dumps(dict(schema=1, fixture=True, mode=mode)))
        (session/'settings.json').write_text('DO-NOT-COLLECT-ROOM-SECRET')
        (session/'characters.txt').write_text('DO-NOT-COLLECT-SAVES')
        child = subprocess.Popen([str(root/'build-v2/RelWithDebInfo/azs_crash_fixture.exe'), str(session.resolve()), mode])
        ready = k32.CreateEventW(None, True, False, f'Local\\AzerothCoopWatchReady-{child.pid}')
        helper = subprocess.Popen([str(root/'bin/Azeroth Survivors Koop.exe'), '/watch-game', str(child.pid), '/session', str(session.resolve()), '/quiet'])
        try:
            assert k32.WaitForSingleObject(ready, 10000) == 0, 'watcher did not become ready'
            go = k32.OpenEventW(2, False, f'Local\\AzerothCoopFixtureGo-{child.pid}')
            assert go
            k32.SetEvent(go); k32.CloseHandle(go)
            if mode == 'kill':
                time.sleep(.3); child.kill()
            child.wait(timeout=20); helper.wait(timeout=20)
            assert helper.returncode == 0
            report = json.loads((session/'exit.json').read_text(encoding='utf-8-sig'))
            assert report['unexpected'] == (mode != 'normal'), report
            archive = session.parent.parent/'crashes'/f'{mode}.zip'
            assert archive.exists() == (mode != 'normal')
            if archive.exists():
                with zipfile.ZipFile(archive) as bundle:
                    assert 'exit.json' in bundle.namelist()
                    assert 'settings.json' not in bundle.namelist() and 'characters.txt' not in bundle.namelist()
            if mode == 'crash':
                dump = json.loads((session/'dump-result.json').read_text(encoding='utf-8-sig'))
                assert dump['success'], dump
                memory = (session/'crash.dmp').read_bytes()
                assert memory[:4] == b'MDMP'
                streams, directory = struct.unpack_from('<II', memory, 8)
                entries = [struct.unpack_from('<III', memory, directory + i*12) for i in range(streams)]
                exception_stream = next(rva for kind, size, rva in entries if kind == 6)
                assert struct.unpack_from('<I', memory, exception_stream+8)[0] == 0xe0421001
                assert any(kind == 3 for kind, size, rva in entries), 'thread list missing'
                assert report['exitCodeHex'] == '0xe0421001' and report['nativeException']
            if mode == 'failfast':
                assert not report['nativeException'] and report['exitCode'] != 0
            result[mode] = report
        finally:
            k32.CloseHandle(ready)
            if child.poll() is None: child.kill(); child.wait()
            if helper.poll() is None: helper.kill(); helper.wait()
    result['passed'] = True
    (args.output/'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))

if __name__ == '__main__': main()
