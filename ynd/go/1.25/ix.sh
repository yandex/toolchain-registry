{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.25rc1
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq '.[] | select(.version=="go1.25rc1") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | .filename, .sha256, ""'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    7588a720e243e4672e0dc1c7942ec7592d480a80440fa2829be8b22c9c44a5b7
{% elif linux and aarch64 %}
    ee0b82bc1421c66f3f322a214218b423beddb64182e0105dbff142e777e96fc1
{% elif darwin and x86_64 %}
    1c0ba988b1457f845bcb3d7efedf4f2fed072fc0d5cad64b90091513df9530d5
{% elif darwin and arm64 %}
    8df00c64e37e13270add03560ba2f11a7e2c498ae1e122065f694d79682c05d4
{% elif mingw32 %}
    0879e884a1300034f7ae48180b12a9fedec861c2c2f94b9af4b6604cf06cc9d7
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
