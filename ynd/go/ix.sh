{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.23.7
{% endblock %}

{% block archive_hash %}
{% if linux and x86_64 %}
    4741525e69841f2e22f9992af25df0c1112b07501f61f741c12c6389fcb119f3
{% elif linux and aarch64 %}
    597acbd0505250d4d98c4c83adf201562a8c812cbcd7b341689a07087a87a541
{% elif darwin and x86_64 %}
    3a3d6745286297cd011d2ab071998a85fe82714bf178dc3cd6ecd3d043a59270
{% elif darwin and arm64 %}
    a08a77374a4a8ab25568cddd9dad5ba7bb6d21e04c650dc2af3def6c9115ebba
{% elif mingw32 %}
    eba0477381037868738b47b0198d120a535eb9a8a17b2babb9ab0d5e912a2171
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
