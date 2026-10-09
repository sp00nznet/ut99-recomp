"""Print the UnrealScript source text a package carries, for one class or a
search. Shipped .u files keep each class's script text; this reads it, to
learn the API a mod of ours calls (UWindow, UMenu, Botpack).

  py -3 tools/uscript.py UMenu UMenuModMenuItem     the class's text
  py -3 tools/uscript.py UMenu -l                   the classes it carries
"""
import os, re, sys

SYS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'System')


def classes(pkg):
    d = open(os.path.join(SYS, pkg + '.u'), 'rb').read()
    # a class's text starts at its declaration and runs to the next NUL
    for m in re.finditer(rb'(?:^|[\x00-\x1f])((?://[^\n]*\n|\s)*class\s+(\w+)\s+(?:extends|expands)\b[^\x00]*)', d, re.I):
        yield m.group(2).decode('latin-1'), m.group(1).decode('latin-1')


if __name__ == '__main__':
    pkg, what = sys.argv[1], sys.argv[2]
    for name, text in classes(pkg):
        if what == '-l':
            print(name)
        elif name.lower() == what.lower():
            print(text.replace('\r', ''))
