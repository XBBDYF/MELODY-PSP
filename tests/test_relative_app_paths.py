"""Focused regression checks for relocatable application files and saved skins."""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
OWNED = ('app.h', 'main.c', 'api.c', 'audio.c', 'local.c', 'log.c',
         'mv.c', 'tls.c', 'skin_runtime.c', 'skin_runtime.h')


def function(source, name):
    match = re.search(r'(?:static\s+)?int\s+' + name + r'\s*\([^)]*\)\s*\{', source)
    if not match:
        raise AssertionError('missing production helper: ' + name)
    start = match.start()
    pos = match.end()
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


class RelativeAppPaths(unittest.TestCase):
    def test_application_paths_and_version(self):
        header = (ROOT / 'src/app.h').read_text(encoding='utf-8')
        self.assertRegex(header, r'#define\s+APP_DIR\s+"\./"')
        self.assertRegex(header, r'#define\s+APP_VER\s+"4\.1\.1"')
        for name in OWNED:
            with self.subTest(file=name):
                self.assertNotIn('ms0:/PSP/GAME/MELODY/',
                                 (ROOT / 'src' / name).read_text(encoding='utf-8'))
        self.assertIn('ms0:/MUSIC', (ROOT / 'src/local.c').read_text(encoding='utf-8'))

    def test_saved_skin_migration_uses_production_helper(self):
        source = (ROOT / 'src/skin_runtime.c').read_text(encoding='utf-8')
        helper = function(source, 'skin_relative_root')
        compiler = shutil.which('gcc') or shutil.which('cc')
        self.assertIsNotNone(compiler, 'a host C compiler is required')
        program = '#include <stdio.h>\n#include <string.h>\n#define APP_DIR "./"\n'
        program += function(source, 'valid_id') + '\n' + helper
        program += '\nint main(int argc,char **argv){char out[256]; if(argc!=2)return 2; if(skin_relative_root(argv[1],out,sizeof(out))<0)return 1; puts(out);return 0;}\n'
        with tempfile.TemporaryDirectory() as folder:
            cfile = pathlib.Path(folder) / 'migration.c'
            exe = pathlib.Path(folder) / 'migration.exe'
            cfile.write_text(program, encoding='utf-8')
            subprocess.run([compiler, '-Wall', '-Werror', str(cfile), '-o', str(exe)], check=True)
            valid = ['./skins/metro', 'skins/metro',
                     'ms0:/PSP/GAME/MELODY/skins/metro',
                     'ms0:/PSP/GAME/Category/MELODY/skins/metro',
                     '/arbitrary/previous/root/skins/Az_09-x',
                     './skins/' + 'a' * 32]
            invalid = ['', './skins/', './skins/..', './skins/a/child',
                       './skins/a.json', './skins/a/../b', './skins/a\\b',
                       './skins/a ', './skins/é', './skins/' + 'a' * 33,
                       'skinsx/a', './cache/a']
            for value in valid:
                result = subprocess.run([str(exe), value], capture_output=True, text=True)
                with self.subTest(valid=value):
                    self.assertEqual(result.returncode, 0)
                    self.assertEqual(result.stdout.strip(), './skins/' + value.rsplit('/', 1)[-1])
            for value in invalid:
                with self.subTest(invalid=value):
                    self.assertEqual(subprocess.run([str(exe), value], capture_output=True).returncode, 1)
        init = source[source.index('void skin_runtime_init('):source.index('const SkinStyle*skin_runtime_style')]
        self.assertIn('skin_relative_root(', init)
        self.assertIn('skin_runtime_apply(root,', init)


if __name__ == '__main__':
    unittest.main()


