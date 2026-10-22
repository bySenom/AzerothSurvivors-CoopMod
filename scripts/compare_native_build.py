"""Read-only function matching. Relative targets are masked, field offsets are not."""
from pathlib import Path
import argparse, bisect, collections, difflib, hashlib, json, re, struct
import pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind

ROOT = Path(__file__).resolve().parents[1]

class Image:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        self.pe = pefile.PE(data=self.data, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.text = next(s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.code = self.data[self.text.PointerToRawData:self.text.PointerToRawData+self.text.Misc_VirtualSize]
        pdata = next(s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.pdata')
        block = self.data[pdata.PointerToRawData:pdata.PointerToRawData+pdata.Misc_VirtualSize]
        self.functions = {a:b for a,b,_ in struct.iter_unpack('<III', block) if a}
        self.starts = sorted(self.functions)
    def body(self, rva, size=None):
        offset = self.pe.get_offset_from_rva(rva)
        return self.data[offset:offset+(size or self.functions[rva]-rva)]

def normalize(body, rva):
    decoder = Decoder(64, body, ip=rva)
    result = bytearray(body); mask = bytearray(b'\xff'*len(body))
    for instruction in decoder:
        constants = decoder.get_constant_offsets(instruction)
        start = instruction.ip-rva
        if instruction.is_ip_rel_memory_operand:
            first = start+constants.displacement_offset; last=first+constants.displacement_size
            mask[first:last] = b'\0'*(last-first)
        if instruction.op0_kind in (OpKind.NEAR_BRANCH16, OpKind.NEAR_BRANCH32, OpKind.NEAR_BRANCH64):
            first=start+constants.immediate_offset; last=first+constants.immediate_size
            mask[first:last] = b'\0'*(last-first)
    for i, value in enumerate(mask):
        if not value: result[i]=0
    return bytes(result), bytes(mask)

def prefix_candidates(old, new, rva, length):
    body=old.body(rva, min(length, old.functions.get(rva,rva+length)-rva))
    normalized, mask=normalize(body,rva)
    # Use the longest literal part for fast searching in the 90+ MB text section.
    literals=list(re.finditer(b'\xff+',mask))
    if not literals: return []
    chunk=max(literals,key=lambda m:m.end()-m.start())
    needle=body[chunk.start():chunk.end()]
    found=[]; position=0
    while (position:=new.code.find(needle,position))>=0:
        offset=position-chunk.start(); candidate=new.text.VirtualAddress+offset
        if offset>=0 and candidate in new.functions:
            sample=new.code[offset:offset+len(body)]
            if all(not flag or a==b for a,b,flag in zip(body,sample,mask)): found.append(candidate)
        position+=1
    return found

def tokens(image, rva):
    return [(i.code,i.op0_register,i.op1_register,i.op2_register,i.memory_base,i.memory_index)
            for i in Decoder(64,image.body(rva),ip=rva)]

def grams(items): return collections.Counter(tuple(items[i:i+4]) for i in range(len(items)-3))

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--old',type=Path,required=True); ap.add_argument('--new',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True); args=ap.parse_args()
    old,new=Image(args.old),Image(args.new)
    targets={'server_tick':0x58798d0,'pick_request':0x5887c50,'open':0x5878ce0,'write':0x59b4170,
             'drain':0x59b42d0,'drop':0xfdb740,'pair':0x59b5140}
    contract=(ROOT/'src/contracts/legacy/gameplay_contract.hpp').read_text()
    targets.update({name:int(address,16) for name,address in re.findall(r'Entry (\w+) = \{0x([0-9a-f]+),',contract)})
    rows={}; semantic={}
    for start, end in new.functions.items():
        if 0x5800000 <= start <= 0x5b00000:
            seq=tokens(new,start); semantic[start]=(seq, grams(seq))
    for name,rva in targets.items():
        if rva not in old.functions:
            rows[name]={'old':hex(rva),'unwind':False};continue
        oldbody=old.body(rva); normalized,_=normalize(oldbody,rva)
        matches=[]
        for length in (192,96,48,24):
            candidates=prefix_candidates(old,new,rva,length)
            if candidates:break
        for candidate in candidates[:100]:
            body=new.body(candidate); value,_=normalize(body,candidate)
            ratio=1.0 if normalized==value else difflib.SequenceMatcher(None,normalized,value,autojunk=True).ratio()
            matches.append({'rva':hex(candidate),'length':len(body),'normalized_identical':normalized==value,'similarity':round(ratio,5)})
        matches.sort(key=lambda x:x['similarity'],reverse=True)
        oldtokens=tokens(old,rva); oldgrams=grams(oldtokens)
        related=[]
        for candidate,(sequence,counts) in semantic.items():
            if not 0.6*len(oldbody) <= new.functions[candidate]-candidate <= 1.6*len(oldbody): continue
            total=sum(oldgrams.values())+sum(counts.values())
            score=2*sum((oldgrams & counts).values())/total if total else 0
            if score>0.35: related.append({'rva':hex(candidate),'length':new.functions[candidate]-candidate,'instruction_4gram_similarity':round(score,5)})
        related.sort(key=lambda x:x['instruction_4gram_similarity'],reverse=True)
        rows[name]={'old':hex(rva),'old_length':len(oldbody),'candidates':matches[:5],'semantic_candidates':related[:3]}
        print(name, json.dumps(rows[name]), flush=True)
    result={'old_sha256':hashlib.sha256(old.data).hexdigest(),'new_sha256':hashlib.sha256(new.data).hexdigest(),
            'timestamp':hex(new.pe.FILE_HEADER.TimeDateStamp),'image_size':hex(new.pe.OPTIONAL_HEADER.SizeOfImage),'functions':rows}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(result,indent=2),encoding='utf-8')

if __name__=='__main__': main()
