{% extends '//die/std/ix.sh' %}

{% block bld_tool %}
bin/wget
{% endblock %}

{% block use_network %}true{% endblock %}

{% block step_unpack %}
mkdir src
{% endblock %}

{% block predict_outputs %}
[{"path": "go", "sum": ""}]
{% endblock %}

{% block go_version %}
1.22.5
{% endblock %}

{% block go_src %}
{% if linux and x86_64 %}
go{{self.go_version().strip()}}.linux-amd64.tar.gz
{% elif linux and aarch64 %}
go{{self.go_version().strip()}}.linux-arm64.tar.gz
{% elif darwin and x86_64 %}
go{{self.go_version().strip()}}.darwin-amd64.tar.gz
{% elif darwin and arm64 %}
go{{self.go_version().strip()}}.darwin-arm64.tar.gz
{% elif mingw32 %}
go{{self.go_version().strip()}}.windows-amd64.zip
{% endif %}
{% endblock %}

{% block step_build %}
set -xue
wget https://go.dev/dl/{{self.go_src().strip()}}
tar xf {{self.go_src().strip()}} || unzip {{self.go_src().strip()}}
rm {{self.go_src().strip()}} 
cd go
sed -i 's/GOTOOLCHAIN=auto/GOTOOLCHAIN=local/g' go.env
rm -r "test/fixedbugs/issue27836.dir"
{% endblock %}

{% block install %}
mv ${tmp}/go/* ${out}
{% endblock %}

