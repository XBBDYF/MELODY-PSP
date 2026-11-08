"""Compile every public libjpeg record member offset with PSP compiler, no execution."""
from pathlib import Path
import re, subprocess, hashlib, json
ROOT=Path(__file__).resolve().parents[3]
B=ROOT/'.tmp/coverjpeg-slim'
B.mkdir(parents=True, exist_ok=True)
P=ROOT/'third_party/coverjpeg_psp'
CC='E:/pspdev/bin/psp-gcc.exe'
OBJ='E:/pspdev/bin/psp-objcopy.exe'
seed=B/'abi_seed.c'
seed.write_text('#include <stddef.h>\n#include <stdio.h>\n#include <jpeglib.h>\n')
text=subprocess.check_output([CC,'-E','-P',str(seed)], text=True)
names=['jpeg_common_struct','jpeg_compress_struct','jpeg_decompress_struct','jpeg_error_mgr','jpeg_progress_mgr','jpeg_destination_mgr','jpeg_source_mgr','jpeg_memory_mgr','jpeg_marker_struct']
types=['JQUANT_TBL','JHUFF_TBL','jpeg_component_info','jpeg_scan_info']
records=[]
for m in re.finditer(r'(typedef\s+)?struct(?:\s+(\w+))?\s*\{',text):
    pos=m.end(); start=pos; depth=1
    while depth:
        if text[pos]=='{': depth+=1
        elif text[pos]=='}': depth-=1
        pos+=1
    trailer=text[pos:text.index(';',pos)]
    if m.group(2) in names: typ='struct '+m.group(2)
    elif m.group(1) and trailer.strip() in types: typ=trailer.strip()
    else: continue
    body=text[start:pos-1]
    stmts=[]; dep=0; last=0
    for i,c in enumerate(body):
        if c=='{': dep+=1
        elif c=='}': dep-=1
        elif c==';' and not dep:
            stmts.append(body[last:i].strip());last=i+1
    fields=[]
    for st in stmts:
        fp=re.search(r'\(\s*\*\s*(\w+)\s*\)', st)
        if fp: fields.append(fp.group(1));continue
        if '}' in st: st=st[st.rindex('}')+1:]
        for part in st.split(','):
            part=re.sub(r'\[[^\]]*\]','',part)
            ids=re.findall(r'\w+',part)
            assert ids,st
            fields.append(ids[-1])
    records.append((typ,fields))
assert len(records)==13, records
expr=[]; labels=[]
for typ,fields in records:
    expr.append('sizeof('+typ+')');labels.append('sizeof '+typ)
    for field in fields:
        expr.append('offsetof('+typ+','+field+')');labels.append(typ+'.'+field)
source=seed.read_text()+'\nconst unsigned abi_values[] = {\n'+',\n'.join(expr)+'\n};\n'
(B/'abi.c').write_text(source)
results={}
for key,inc in [('sdk','E:/pspdev/psp/include'),('public',str(P/'include')),('private',str(P/'private-src'))]:
    args=[CC,'-c',str(B/'abi.c'),'-o',str(B/f'abi-{key}.o'),'-I'+inc]
    if key=='private': args+=['-I'+str(P/'include')]
    subprocess.run(args,check=True)
    subprocess.run([OBJ,'-O','binary','-j','.rodata',str(B/f'abi-{key}.o'),str(B/f'abi-{key}.bin')],check=True)
    results[key]=(B/f'abi-{key}.bin').read_bytes()
assert results['sdk']==results['public']==results['private']
values=[int.from_bytes(results['sdk'][i:i+4],'little') for i in range(0,len(expr)*4,4)]
report={'compiler':CC,'record_count':len(records),'sizeof_and_offset_count':len(expr),'all_equal':True,'offsets':dict(zip(labels,values))}
(B/'abi-results.json').write_text(json.dumps(report,indent=2))
print(f'ABI PASS: {len(records)} public records; {len(expr)} sizes/offsets byte-identical for SDK, public and private headers')
