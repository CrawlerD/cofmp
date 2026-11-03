# Gera o SHA256SUMS da release: "hash  nome" por linha, LF, na ordem dada.
# Uso: py tools/make_manifest.py <pasta> <arquivo> [arquivo...]
# Arquivos que nao existem sao pulados com aviso (o zip da release tambem so leva os que existem).
import hashlib
import os
import sys


def main(argv):
    if len(argv) < 2:
        print("uso: py tools/make_manifest.py <pasta> <arquivo> [arquivo...]")
        return 2
    folder = argv[0]
    out = os.path.join(folder, "SHA256SUMS")
    lines = []
    for name in argv[1:]:
        path = os.path.join(folder, name)
        if not os.path.isfile(path):
            print(f"aviso: {name} nao existe em {folder}; fora do manifesto")
            continue
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        lines.append(f"{h.hexdigest()}  {name}\n")
    if not lines:
        print("ERRO: nenhum arquivo para o manifesto")
        return 1
    with open(out, "w", newline="\n") as f:
        f.writelines(lines)
    print(f"{len(lines)} arquivos no manifesto: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
