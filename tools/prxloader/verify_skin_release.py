"""Inspect compiled PRX and bundled update, not only restored source headers."""
from pathlib import Path
import re,struct,zlib,sys
BASE=b'http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/skins/'
def verify_prx(raw,allow_acceptance=False):
 markers=[b'NATIVE80 DONE',b'GE79 DONE',b'ALLNAV78 DONE',b'METRO77 DONE',b'METRO75 DONE',b'SKIN73 DONE',b'COS76 DONE',b'FEATURE71 DONE',b'BG72 DONE',b'CACHE74 DONE']
 if not allow_acceptance and any(x in raw for x in markers):raise ValueError('Temporary acceptance-control build leaked into release')
 urls=re.findall(rb'http://[^\x00\s]{1,220}',raw)
 if BASE not in urls:raise ValueError('Production COS skin base missing from compiled melody.prx; recompile src/skin_catalog.o')
 local=[x for x in urls if re.match(rb'http://(?:127\.|localhost[:/]|0\.0\.0\.0[:/])',x)]
 if local:raise ValueError('Test HTTP address leaked into release: '+repr(local))
 return True
def verify_update(raw):
 if raw[:4]!=b'\x00PBP':raise ValueError('Invalid PBP')
 offset=struct.unpack_from('<8I',raw,8)[7]
 if raw[offset:offset+8]!=b'MLDBNDL1':raise ValueError('Missing update bundle')
 magic,version,count,size,crc,_,_=struct.unpack_from('<8s6I',raw,offset)
 if version!=2 or count!=8:raise ValueError('Unsupported update bundle')
 body=raw[offset+32:offset+size]
 if zlib.crc32(body)!=crc:raise ValueError('Bundle CRC mismatch')
 for i in range(count):
  name,n,checksum,at,packed=struct.unpack_from('<56s4I',raw,offset+32+i*72)
  if name.split(b'\0',1)[0]==b'melody.prx':
   module=zlib.decompress(raw[offset+at:offset+at+packed]);assert len(module)==n and zlib.crc32(module)==checksum;return verify_prx(module)
 raise ValueError('No main module')
if __name__=='__main__':
 for arg in sys.argv[1:]:
  path=Path(arg);raw=path.read_bytes();verify_update(raw)if raw[:4]==b'\x00PBP'else verify_prx(raw);print('PASS production compiled skin URL:',path)
