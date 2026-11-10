{% extends '//die/c/ix.sh' %}

{% block version %}
e581929d3d0cf44e033e47bf3b75a2544918b87e
{% endblock %}

{% block pkg_name %}
win32-openssh-full
{% endblock %}

{% block fetch %}
https://github.com/PowerShell/openssh-portable/archive/{{self.version().strip()}}.tar.gz
4f40c596c4cd21ff87caeead23326af3b57bffa73ba5aaabe7d2a59d3a034c33
{% endblock %}

{% block bld_libs %}
lib/c
lib/z
lib/openssl
{% endblock %}

{% block bld_tool %}
bld/python
{% endblock %}

{% block patch %}
{{super()}}
cat > af_unix.patch <<'PATCH'
{% include 'af_unix.patch' %}
PATCH
patch -p1 < af_unix.patch
{% endblock %}

{% block build %}
cat > build_mingw.py <<'PY'
{% include 'build_mingw.py' %}
PY
cat > mingw_compat.h <<'C'
{% include 'mingw_compat.h' %}
C
cat > telemetry_stub.c <<'C'
{% include 'telemetry_stub.c' %}
C
cat > link_compat.c <<'C'
{% include 'link_compat.c' %}
C
python3 build_mingw.py "${out}"
{% endblock %}

{% block install %}
for name in ssh sshd sshd-auth sshd-session scp sftp sftp-server ssh-add ssh-agent ssh-keygen ssh-keyscan ssh-pkcs11-helper ssh-shellhost; do
    test -s "${out}/bin/${name}.exe"
done
{% endblock %}
