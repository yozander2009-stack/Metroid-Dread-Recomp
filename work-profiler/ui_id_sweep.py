"""ui_id_sweep.py EXE [INSTALL]: every Play-screen page in each update state; fails if any clickable item shares an ID
in one frame ("ui duplicate id", runtime/dread_ui/ui.cpp ui_button). States: normal, update available (a local
manifest with a newer version), rebuild needed (setup.ok's patch hash changed for the run), and "dup" (the #72 state,
SWR_FE_TEST_DUP=1, which must be reported: proves the check works). Test install only; restores what it changes."""
import functools, http.server, os, shutil, subprocess, sys, tempfile, threading
EXE = sys.argv[1]
D = sys.argv[2] if len(sys.argv) > 2 else r'C:\Users\yozan\Downloads\MetroidDreadRecomp-test\rel060\Metroid Dread'
if 'games' in os.path.normpath(D).lower().split(os.sep): raise SystemExit('refusing a games folder')
G = os.path.join(D, 'Metroid Dread.exe'); keep = G + '.idsweep'
shutil.copy2(G, keep); shutil.copy2(EXE, G)
ok_path = os.path.join(D, 'Metroid Dread_data', 'setup.ok'); ok0 = open(ok_path, encoding='utf-8').read()
srv_dir = tempfile.mkdtemp()
open(os.path.join(srv_dir, 'update.txt'), 'w').write(
    'version 9.9.9\ntag v9.9.9\npackage update-9.9.9.zip ' + '0' * 64 + ' 1\nfile ' + '0' * 64 + ' 1 Metroid Dread.exe\n')
srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(http.server.SimpleHTTPRequestHandler, directory=srv_dir))
threading.Thread(target=srv.serve_forever, daemon=True).start()
res = os.path.join(srv_dir, 'result.txt')
fails, n = [], 0
try:
    for state in ('normal', 'available', 'rebuild', 'dup'):
        if state == 'rebuild': open(ok_path, 'w', encoding='utf-8').write(ok0.replace('"patches_sha256": "', '"patches_sha256": "00'))
        else: open(ok_path, 'w', encoding='utf-8').write(ok0)
        for page in range(1, 10):
            if os.path.exists(res): os.remove(res)
            env = {k: v for k, v in os.environ.items() if not k.startswith('SWR_')}
            env.update(SWR_FE_TEST='1', SWR_FE_PAGE=str(page), SWR_FE_EXIT_FRAME='150', SWR_FE_RESULT=res)
            if state in ('available', 'dup'): env['SWR_UPDATE_URL'] = 'http://127.0.0.1:%d' % srv.server_address[1]
            if state == 'dup': env['SWR_FE_TEST_DUP'] = '1'
            subprocess.run([G], cwd=D, env=env, timeout=120, capture_output=True)
            dups = [l for l in (open(res).read().splitlines() if os.path.exists(res) else []) if l.startswith('ui duplicate id')]
            n += 1
            if state == 'dup':
                if page == 1 and not dups: fails.append('dup state: the #72 duplicate was NOT reported')
                if page == 1 and dups: print('dup state, play page: reported as expected:', dups[0])
            elif dups: fails.append('%s, page %d: %s' % (state, page, '; '.join(dups)))
finally:
    open(ok_path, 'w', encoding='utf-8').write(ok0); shutil.copy2(keep, G); os.remove(keep); srv.shutdown()
print('%d runs; %s' % (n, 'PASS: no duplicate IDs' if not fails else 'FAIL'))
for f in fails: print('  ', f)
sys.exit(1 if fails else 0)
