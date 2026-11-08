"""Optional actual-engine validation; builds the bounded production Lua runtime once.
Usage: python tools/validate_skin_runtime.py path/to/skin [--rebuild]
Requires host GCC configured by the MELODY developer workspace.
"""
from pathlib import Path
import subprocess,sys
R=Path(__file__).resolve().parents[1];exe=R/'.tmp/skin-runtime-host/runtime.exe'
args=[x for x in sys.argv[1:]if x!='--rebuild']
if len(args)!=1:raise SystemExit('Usage: python tools/validate_skin_runtime.py SKIN_DIRECTORY [--rebuild]')
if not exe.exists()or'--rebuild'in sys.argv:
 subprocess.run([sys.executable,'-B',R/'tests/test_skin_runtime.py'],cwd=R,check=True)
raise SystemExit(subprocess.run([exe,'--validate',str(Path(args[0]).resolve())],cwd=R).returncode)
