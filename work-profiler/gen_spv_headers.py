"""Compile our own Vulkan shaders (runtime/snxvk/src_new/nx/gpu/vk/shaders) with glslang 16.6.0 into the headers the
renderer includes: hdr_out.{vert,frag} -> hdr_spv.h (HDR / FXAA present pass), dlss_mv.{vert,frag} -> dlss_mv_spv.h
(DLSS motion vectors), shrink.frag -> shrink_spv.h (the frame shrunk to the window; its vertex shader is hdr_out's).
glslang: GLSLANG=path, or on PATH, or third_party/glslang/bin.
Usage: gen_spv_headers.py [--check]   (--check: compile and compare with the headers, write nothing)"""
import os, shutil, struct, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VK = os.path.join(ROOT, 'runtime', 'snxvk', 'src_new', 'nx', 'gpu', 'vk')
SH = os.path.join(VK, 'shaders')
GLSLANG = (os.environ.get('GLSLANG') or shutil.which('glslang')
           or os.path.join(ROOT, 'third_party', 'glslang', 'bin', 'glslang.exe'))
HEADERS = [('hdr_spv.h', 'hdr_out', 'g_hdr_out',
            '/* generated from hdr_out.vert / hdr_out.frag by glslang 16.6.0 (glslang -V); see work-profiler/gen_spv_headers.py */'),
           ('dlss_mv_spv.h', 'dlss_mv', 'g_dlss_mv',
            '/* generated from dlss_mv.vert / dlss_mv.frag by glslang 16.6.0 (glslang -V); see work-profiler/gen_spv_headers.py */'),
           ('shrink_spv.h', 'shrink', 'g_shrink',
            '/* generated from shaders/shrink.frag by glslang (glslang -V); see work-profiler/gen_spv_headers.py */')]
STAGES = {'shrink': ('frag',)}   # the others have a vertex and a fragment shader


def words(src):
    with tempfile.TemporaryDirectory() as t:
        out = os.path.join(t, 'o.spv')
        subprocess.run([GLSLANG, '-V', src, '-o', out], check=True, capture_output=True)
        d = open(out, 'rb').read()
    return struct.unpack('<%dI' % (len(d) // 4), d)


def parse(path):   # the word arrays of an existing header, by name
    arrays, cur = {}, None
    for line in open(path):
        if line.startswith('static const unsigned int'):
            cur = line.split()[4].split('[')[0]; arrays[cur] = []
        elif cur and line.strip().startswith('0x'):
            arrays[cur] += [int(x, 16) for x in line.replace(',', ' ').split()]
    return arrays


check = '--check' in sys.argv
ok = True
for header, base, sym, comment in HEADERS:
    made = {sym + '_' + st: words(os.path.join(SH, '%s.%s' % (base, st))) for st in STAGES.get(base, ('vert', 'frag'))}
    hp = os.path.join(VK, header)
    if check:
        have = parse(hp)
        for k, w in made.items():
            same = list(w) == have.get(k)
            ok &= same
            print('%-14s %-16s %s (%d words)' % (header, k, 'identical' if same else 'DIFFERENT', len(w)))
        continue
    out = [comment]
    for k, w in made.items():
        out.append('static const unsigned int %s[%d] = {' % (k, len(w)))
        for i in range(0, len(w), 8): out.append('    ' + ', '.join('0x%08x' % x for x in w[i:i + 8]) + ',')
        out.append('};')
    open(hp, 'w', newline='\n').write('\n'.join(out) + '\n')
    print(header, 'written')
sys.exit(0 if ok else 1)
