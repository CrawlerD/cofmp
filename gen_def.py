# Gera os exports do cofmp.dll: nossas 3 funcoes (cofmp.def) e o resto encaminhado
# para hl.dll (exports_fwd.h, via #pragma /EXPORT - o .def nao encaminha nomes C em x86).
# Uso: py gen_def.py <caminho do hl.dll> <pasta de saida>
import os
import sys
import pefile

OWN = {"GiveFnptrsToDll", "GetEntityAPI", "GetEntityAPI2"}

pe = pefile.PE(sys.argv[1], fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
names = sorted(e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name)
fwd = [n for n in names if n not in OWN]

out = sys.argv[2]
with open(os.path.join(out, "cofmp.def"), "w", newline="\n") as f:
    f.write("LIBRARY cofmp\nEXPORTS\n    GiveFnptrsToDll\n    GetEntityAPI\n    GetEntityAPI2\n")
with open(os.path.join(out, "exports_fwd.h"), "w", newline="\n") as f:
    for n in fwd:
        f.write(f'#pragma comment(linker, "/EXPORT:{n}=hl.{n}")\n')

print(f"{len(fwd)} exports encaminhados")
