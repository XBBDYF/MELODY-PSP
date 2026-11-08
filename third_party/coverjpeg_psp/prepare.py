"""Prepare a private decode-only 8-bit libjpeg-turbo 3.1.3 source overlay.
The complete, unmodified upstream tree is retained beside the generated overlay.
"""
from pathlib import Path
import difflib
import shutil

root = Path(__file__).resolve().parent
up = root / 'libjpeg-turbo-3.1.3' / 'src'
out = root / 'private-src'
out.mkdir(exist_ok=True)
for p in up.iterdir():
    if p.is_file():
        shutil.copyfile(p, out / p.name)
shutil.copytree(up / 'wrapper', out / 'wrapper', dirs_exist_ok=True)
patches = []
def patch(name, transform):
    old = (up / name).read_text()
    new = transform(old)
    assert new != old, name
    new = ('/* CoverJPEG PSP decode-only modifications.\n'
           ' * Copyright (C) 2026 MELODY project contributors.\n'
           ' * Original upstream copyright and license notices follow unchanged.\n'
           ' */\n') + new
    (out / name).write_text(new, newline='\n')
    patches.extend(difflib.unified_diff(old.splitlines(True), new.splitlines(True), fromfile='upstream/src/'+name, tofile='private-src/'+name))

def caps(s):
    for cap in ['DCT_IFAST_SUPPORTED', 'DCT_FLOAT_SUPPORTED', 'C_MULTISCAN_FILES_SUPPORTED', 'C_PROGRESSIVE_SUPPORTED', 'C_LOSSLESS_SUPPORTED', 'ENTROPY_OPT_SUPPORTED', 'INPUT_SMOOTHING_SUPPORTED', 'D_LOSSLESS_SUPPORTED', 'QUANT_1PASS_SUPPORTED', 'QUANT_2PASS_SUPPORTED']:
        s = s.replace('#define ' + cap, '#undef  ' + cap)
    return s
patch('jmorecfg.h', caps)
patch('jdinput.c', lambda s: s.replace('cinfo->data_precision != 8 && cinfo->data_precision != 12', 'cinfo->data_precision != 8'))

def master(s):
    # Never dispatch 12-bit modules.  The header parser and this second gate
    # reject other precisions before output allocation or module selection.
    needle = '  /* Initialize dimensions and other stuff */'
    s = s.replace(needle, '  /* Private 8-bit decode build: reject unsupported precision explicitly. */\n  if (cinfo->data_precision != 8)\n    ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);\n\n' + needle)
    import re
    s, n = re.subn(r'\n( *)else if \(cinfo->data_precision (?:==|<=) 12\)\n *j12init_[\s\S]*?;[^\n]*', '', s)
    assert n == 8, n
    old = '''      } else if (cinfo->data_precision <= 12) {
        j12init_color_deconverter(cinfo);
        j12init_upsampler(cinfo);'''
    assert old in s
    s = s.replace(old, '')
    old = '''    if (cinfo->data_precision == 12)
      j12init_d_coef_controller(cinfo, use_c_buffer);
    else
      jinit_d_coef_controller(cinfo, use_c_buffer);'''
    assert old in s
    s = s.replace(old, '    jinit_d_coef_controller(cinfo, use_c_buffer);')
    return s
patch('jdmaster.c', master)
(root / 'decode-only.patch').write_text(''.join(patches), newline='\n')
config = root / 'include'
config.mkdir(exist_ok=True)
# Public header is upstream, not a reconstructed ABI.  Match SDK config and
# remove arithmetic capabilities only (these macros do not alter public layout).
sdk = (root / 'config/jconfig.h').read_text()
sdk = sdk.replace('#define C_ARITH_CODING_SUPPORTED 1', '/* #undef C_ARITH_CODING_SUPPORTED */').replace('#define D_ARITH_CODING_SUPPORTED 1', '/* #undef D_ARITH_CODING_SUPPORTED */')
(config / 'jconfig.h').write_text(sdk, newline='\n')
for name in ['jpeglib.h', 'jmorecfg.h', 'jerror.h']:
    shutil.copyfile(up / name, config / name)
# Compiler predefined sizeof preserves host/target equivalent configuration.
(config / 'jconfigint.h').write_text('''#define BUILD "20261105-coverjpeg-decode8"
#define HIDDEN __attribute__((visibility("hidden")))
#undef inline
#define INLINE __inline__ __attribute__((always_inline))
#define THREAD_LOCAL __thread
#define PACKAGE_NAME "libjpeg-turbo"
#define VERSION "3.1.3"
#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__
#define HAVE_BUILTIN_CTZL
#define FALLTHROUGH __attribute__((fallthrough));
#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE 8
#endif
#undef C_ARITH_CODING_SUPPORTED
#undef D_ARITH_CODING_SUPPORTED
#undef WITH_SIMD
''', newline='\n')
(config / 'jversion.h').write_text((up / 'jversion.h.in').read_text().replace('@COPYRIGHT_YEAR@', '2009-2024'), newline='\n')
print('Prepared pinned 3.1.3 private decode overlay; public ABI headers unchanged.')
