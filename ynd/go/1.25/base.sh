{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.25.4
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.25.0") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    9fa5ffeda4170de60f67f3aa0f824e426421ba724c21e133c1e35d6159ca1bec
{% elif linux and aarch64 %}
    a68e86d4b72c2c2fecf7dfed667680b6c2a071221bbdb6913cf83ce3f80d9ff0
{% elif darwin and x86_64 %}
    33ba03ff9973f5bd26d516eea35328832a9525ecc4d169b15937ffe2ce66a7d8
{% elif darwin and arm64 %}
    c1b04e74251fe1dfbc5382e73d0c6d96f49642d8aebb7ee10a7ecd4cae36ebd2
{% elif mingw32 %}
    6dad204d42719795f22067553b2b042c0e710b32c5a00f6c67892865167fdfd0
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
(base64 -d | patch -p1) << EOF
{{ix.load_file('//go/1.25/old_coverage.diff') | b64e}}
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
