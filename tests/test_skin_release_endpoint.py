"""Release gate must inspect bundled machine bytes; source restoration alone is insufficient."""
from pathlib import Path
import importlib.util,struct,zlib,unittest
R=Path(__file__).resolve().parents[1];spec=importlib.util.spec_from_file_location('skin_release_gate',R/'tools/prxloader/verify_skin_release.py');gate=importlib.util.module_from_spec(spec);spec.loader.exec_module(gate)
def bundle(module):
 packed=zlib.compress(module);at=32+8*72;entries=bytearray();body=bytearray()
 for i in range(8):
  raw=module if i==0 else b'module';p=zlib.compress(raw);name=b'melody.prx'if i==0 else('other%d.prx'%i).encode();entries+=struct.pack('<56s4I',name,len(raw),zlib.crc32(raw),at,len(p));body+=p;at+=len(p)
 payload=entries+body;header=struct.pack('<8s6I',b'MLDBNDL1',2,8,32+len(payload),zlib.crc32(payload),0,0);loader=b'\x00PBP'+struct.pack('<I8I',1,*([40]*8));return loader+header+payload
class ReleaseEndpoint(unittest.TestCase):
 def test_official_machine_bytes_and_update_pass(self):
  good=b'ELF'+gate.BASE+b'\0';self.assertTrue(gate.verify_prx(good));self.assertTrue(gate.verify_update(bundle(good)))
 def test_restored_header_cannot_hide_stale_test_object(self):
  bad=b'ELFhttp://127.0.0.1:4804/\0';self.assertIn(gate.BASE.decode(),(R/'src/skin_catalog.h').read_text(encoding='utf-8'))
  with self.assertRaises(ValueError):gate.verify_prx(bad)
  with self.assertRaises(ValueError):gate.verify_update(bundle(bad))
 def test_acceptance_control_binary_is_not_a_production_release(self):
  with self.assertRaises(ValueError):gate.verify_prx(gate.BASE+b'\0METRO77 DONE\0')
 def test_even_mixed_official_and_test_addresses_are_blocked(self):
  with self.assertRaises(ValueError):gate.verify_prx(gate.BASE+b'\0http://127.0.0.1:4804/\0')
if __name__=='__main__':unittest.main()
