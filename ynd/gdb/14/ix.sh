{% extends '//bin/gdb/reloc/ix.sh' %}

{% block gdb_args %}--eval-command="source \${p2}/share/gdb/python/arc/__init__.py"{% endblock %}

{% block pretty_printers %}
__init__.py
arcadia_printers.py
arcadia_xmethods.py
libc_printers.py
libcxx_printers.py
libcxx_xmethods.py
libpython_printers.py
libstdcpp_printers.py
tcont_printer.py
yabs_printers.py
yt_fibers_printer.py
{% endblock %}

{% block install %}
{{super()}}

sed -e 's|/usr/bin/env bash|/bin/bash|' -i ${out}/bin/gcore

cd ${out}/share/gdb/python
mkdir arc; cd arc
{% for pp in self.pretty_printers().strip().split() %}
base64 -d << EOF > {{pp}}
{{ix.load_file('pretty_printers/' + pp) | b64e}}
EOF
{% endfor %}

mkdir ${out}/fix
cat << EOF > ${out}/fix/mksymlink.sh
mkdir gdb
mv bin lib share gdb/
EOF
{% endblock %}
