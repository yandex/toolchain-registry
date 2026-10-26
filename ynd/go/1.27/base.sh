{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.27.3
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.27.3") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40
{% elif linux and aarch64 %}
    a06e99309dbcbdca1a8ccd2282c3ee146e55930886e26b4f1193269587165f49
{% elif darwin and x86_64 %}
    b6f8e931e4ab497fcde6da8ae4499a58f9d258e55c8a80824a516eae5950a750
{% elif darwin and arm64 %}
    dd3908cf20959d798c1e06ebe5b72213f0505ce01373b40ce3110975a6cd99c2
{% elif mingw32 %}
    f26081f438319757f8a1b46d7d0b2c5ec7583d4038e485e0aa725cfde7885fff
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

{% block fetch %}
https://go.dev/dl/go{{self.go_version().strip()}}.{{self.archive_name().strip()}}
sha:{{self.archive_hash().strip()}}
{% endblock %}

{% block step_build %}
sed -i 's/GOTOOLCHAIN=auto/GOTOOLCHAIN=local/g' go.env
rm -r "test/fixedbugs/issue27836.dir"
{% endblock %}
