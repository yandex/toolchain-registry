{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.25.5
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.25.5") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    9e9b755d63b36acf30c12a9a3fc379243714c1c6d3dd72861da637f336ebb35b
{% elif linux and aarch64 %}
    b00b694903d126c588c378e72d3545549935d3982635ba3f7a964c9fa23fe3b9
{% elif darwin and x86_64 %}
    b69d51bce599e5381a94ce15263ae644ec84667a5ce23d58dc2e63e2c12a9f56
{% elif darwin and arm64 %}
    bed8ebe824e3d3b27e8471d1307f803fc6ab8e1d0eb7a4ae196979bd9b801dd3
{% elif mingw32 %}
    ae756cce1cb80c819b4fe01b0353807178f532211b47f72d7fa77949de054ebb
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
