{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.24.4
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq '.[] | select(.version=="go1.24.4") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | .filename, .sha256, ""'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    77e5da33bb72aeaef1ba4418b6fe511bc4d041873cbf82e5aa6318740df98717
{% elif linux and aarch64 %}
    d5501ee5aca0f258d5fe9bfaed401958445014495dc115f202d43d5210b45241
{% elif darwin and x86_64 %}
    69bef555e114b4a2252452b6e7049afc31fbdf2d39790b669165e89525cd3f5c
{% elif darwin and arm64 %}
    27973684b515eaf461065054e6b572d9390c05e69ba4a423076c160165336470
{% elif mingw32 %}
    b751a1136cb9d8a2e7ebb22c538c4f02c09b98138c7c8bfb78a54a4566c013b1
{% endif %}
{% endblock %}

{% block archive_name %}
{% if linux and x86_64 %}
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

{% block install %}
mv ${tmp}/src/* ${out}
{% endblock %}
