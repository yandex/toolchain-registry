{% extends '//die/go/build.sh' %}

{% block go_url %}
https://github.com/go-yaml/yaml/archive/refs/tags/v3.0.1.tar.gz
{% endblock %}

{% block go_sha %}
f99272ba651b0bca3d9bfc160297b7c6985ad90ca6bdb1b0fd339bf02da23355
{% endblock %}

{% block patch %}
{{super()}}

# Rename package
grep -r 'package yaml$' -l | xargs -L1 sed -i 's|package yaml|package main|g'
# Remove tests
find . -name '*_test.go' -delete

# Insert main.go
base64 -d << EOF > ${tmp}/src/main.go
{{ix.load_file('//ynd/bin/yaml2json/main.go') | b64e }}
EOF
{% endblock %}


{% block go_build_flags %}
-o yaml2json
{% endblock %}

{% block install %}
mkdir ${out}/bin
cp yaml2json ${out}/bin
{% endblock %}
