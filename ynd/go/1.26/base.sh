{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.26.3
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.26.3") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    2b2cfc7148493da5e73981bffbf3353af381d5f93e789c82c79aff64962eb556
{% elif linux and aarch64 %}
    9d89a3ea57d141c2b22d70083f2c8459ba3890f2d9e818e7e933b75614936565
{% elif darwin and x86_64 %}
    278d580b32e299fe4a9c990fcf2d02acfe538c7e551a6ee18f9c7164573d2c63
{% elif darwin and arm64 %}
    875cf54a15311eee2c99b9dd67c68c4a49351d489ab622bf2cfd28c8f2078d3c
{% elif mingw32 %}
    20d2ceafb4ed41b96b879010927b28bc92a5be57a7c1801ce365a9ca51d3224a
{% endif %}
{% endblock %}

{% block archive_name %}
{% if linux and x86_64 or build_tool %}
    linux-amd64.tar.gz
{% elif linux and aarch64 %}
    linux-arm64.tar.gz
{% elif darwin and x86_64 %}
    darwin-amd64.tar.gz
{% elif darwin and arm64 %}
    darwin-arm64.tar.gz
{% elif mingw32 %}
    windows-amd64.zip
{% endif %}
{% endblock %}

{% block step_patch %}
(base64 -d | patch -p1 -r /dev/null --no-backup-if-mismatch) << EOF
{{ix.load_file('//go/1.26/old_coverage.diff') | b64e}}
EOF
{% endblock %}

{% block fetch %}
https://go.dev/dl/go{{self.go_version().strip()}}.{{self.archive_name().strip()}}
sha:{{self.archive_hash().strip()}}
{% endblock %}

{% block step_build %}
sed -i 's/GOTOOLCHAIN=auto/GOTOOLCHAIN=local/g' go.env
rm -r "test/fixedbugs/issue27836.dir"
{% endblock %}
