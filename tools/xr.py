import sys,pefile,struct
pe=pefile.PE('DSLOA.exe',fast_load=True)
base=pe.OPTIONAL_HEADER.ImageBase
text=[s for s in pe.sections if s.Name.startswith(b'.text')][0]
data=text.get_data(); tb=base+text.VirtualAddress
def calls(t):
    r=[]
    for i in range(len(data)-5):
        if data[i] in (0xe8,0xe9):
            if (tb+i+5+struct.unpack_from('<i',data,i+1)[0])&0xffffffff==t: r.append(tb+i)
    return r
def refs(v):
    p=struct.pack('<I',v); r=[]; i=data.find(p)
    while i>=0: r.append(tb+i); i=data.find(p,i+1)
    return r
if __name__=='__main__':
    for a in sys.argv[1:]:
        if a[0]=='c': print(a,[hex(x) for x in calls(int(a[1:],16))])
        else: print(a,[hex(x) for x in refs(int(a[1:],16))])
