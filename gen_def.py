# Gera os exports do cofmp.dll: nossas 3 funcoes (cofmp.def) e o resto encaminhado
# para hl.dll (exports_fwd.h, via #pragma /EXPORT - o .def nao encaminha nomes C em x86).
#
# Uso: py gen_def.py <caminho do hl.dll | -> <pasta de saida>
#      py gen_def.py --list <caminho do hl.dll> [saida.txt]
#
# Com "-" (ou quando o hl.dll nao existe), usa tools/hl_exports.txt. Assim o build e a CI
# nao dependem do hl.dll proprietario do jogo; so o --list precisa dele.
import os
import sys

OWN = {"GiveFnptrsToDll", "GetEntityAPI", "GetEntityAPI2"}
HERE = os.path.dirname(os.path.abspath(__file__))
EXPORTS_TXT = os.path.join(HERE, "tools", "hl_exports.txt")


def ReadExports(dll):
    import pefile  # so necessario para ler o hl.dll de verdade

    pe = pefile.PE(dll, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    return sorted(e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name)


def ReadList(path):
    with open(path, encoding="utf-8") as f:
        return sorted(line.strip() for line in f if line.strip())


def WriteList(names, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        for n in names:
            f.write(n + "\n")


def WriteDef(names, out):
    fwd = [n for n in names if n not in OWN]
    with open(os.path.join(out, "cofmp.def"), "w", newline="\n") as f:
        f.write("LIBRARY cofmp\nEXPORTS\n    GiveFnptrsToDll\n    GetEntityAPI\n    GetEntityAPI2\n")
    with open(os.path.join(out, "exports_fwd.h"), "w", newline="\n") as f:
        for n in fwd:
            f.write(f'#pragma comment(linker, "/EXPORT:{n}=hl.{n}")\n')
    print(f"{len(fwd)} exports encaminhados")


def main(argv):
    if len(argv) >= 2 and argv[0] == "--list":
        names = ReadExports(argv[1])
        out = argv[2] if len(argv) > 2 else EXPORTS_TXT
        WriteList(names, out)
        print(f"{len(names)} exports gravados em {out}")
        return 0
    if len(argv) != 2:
        print("uso: py gen_def.py <hl.dll|-> <pasta de saida>")
        print("     py gen_def.py --list <hl.dll> [saida.txt]")
        return 2
    dll, out = argv
    if dll != "-" and os.path.isfile(dll):
        names = ReadExports(dll)
    else:
        if not os.path.isfile(EXPORTS_TXT):
            print(f"ERRO: hl.dll nao informado e {EXPORTS_TXT} nao existe")
            return 1
        names = ReadList(EXPORTS_TXT)
        print(f"usando a lista fixa de exports ({len(names)} nomes)")
    os.makedirs(out, exist_ok=True)
    WriteDef(names, out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
