{% extends '//die/go/build.sh' %}

{% block pkg_name %}
yamlfmt
{% endblock %}

{% block version %}
0.17.2
{% endblock %}

{% block go_url %}
https://github.com/google/yamlfmt/archive/refs/tags/v{{self.version().strip()}}.tar.gz
{% endblock %}

{% block go_sha %}
5d385a4166c55a6c3a01148b1084e97d9faf21ca1c20771f8640a28990a88ac6
{% endblock %}

{% block go_bins %}
{% if mingw32 %}
yamlfmt.exe
{% else %}
yamlfmt
{% endif %}
{% endblock %}

{% block build %}
{% if mingw32 %}
GOOS=windows
{% endif %}
{{super()}}
{% endblock %}

{% block go_build_flags %}
{% if mingw32 %}
-o yamlfmt.exe ./cmd/yamlfmt
{% else %}
-o yamlfmt ./cmd/yamlfmt
{% endif %}
{% endblock %}

{% block go_tool %}
bin/go/lang/23
{% endblock %}

{% block install %}
mkdir -p ${out}/bin
cp {{self.go_bins().strip()}} ${out}/bin
{{super()}}
{% endblock %}


