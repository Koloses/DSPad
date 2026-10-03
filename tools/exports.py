import pefile
pe=pefile.PE('DSLOA.exe')
with open('exports.txt','w') as f:
    for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        f.write("%08x %s\n"%(pe.OPTIONAL_HEADER.ImageBase+s.address,(s.name or b'').decode()))
