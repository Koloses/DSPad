import sys,pefile,capstone
pe=pefile.PE('DSLOA.exe',fast_load=True)
base=pe.OPTIONAL_HEADER.ImageBase
names={}
for l in open('exports.txt'):
    a,n=l.split(' ',1); names.setdefault(int(a,16),n.strip()[:70])
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
def dis(addr,maxn=60,stop_ret=True):
    data=pe.get_data(addr-base,maxn*8)
    n=0
    for i in md.disasm(data,addr):
        extra=''
        for tok in i.op_str.replace('[',' ').replace(']',' ').split():
            try:
                v=int(tok,16)
                if v in names: extra='  ; '+names[v]
            except: pass
        print("%08x %-7s %s%s"%(i.address,i.mnemonic,i.op_str,extra))
        n+=1
        if n>=maxn or (stop_ret and i.mnemonic in('ret','retn','int3')): break
if __name__=='__main__':
    for a in sys.argv[1:]:
        if ':' in a: ad,n=a.split(':'); 
        else: ad,n=a,60
        print('---',ad, names.get(int(ad,16),'')); dis(int(ad,16),int(n))
