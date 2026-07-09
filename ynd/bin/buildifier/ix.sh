{% extends '//die/go/build.sh' %}

{% block pkg_name %}
buildifier
{% endblock %}

{% block version %}
8.5.1
{% endblock %}

{% block go_url %}
https://github.com/bazelbuild/buildtools/archive/refs/tags/v{{self.version().strip()}}.tar.gz
{% endblock %}

{% block go_sha %}
fe8a56c70734e09e0f894f0cd54906d1004f36ebb745fd8e778395f8464c57ec
{% endblock %}

{% block go_bins %}
{% if mingw32 %}
buildifier.exe
{% else %}
buildifier
{% endif %}
{% endblock %}

{% block go_refine %}
find . -type d -path '*/warn/docs' -exec rm -rf {} + || true
{% endblock %}

{% block build %}
{% if mingw32 %}
GOOS=windows
{% endif %}
{{super()}}
{% endblock %}

{% block go_build_flags %}
{% if mingw32 %}
-o buildifier.exe ./buildifier
{% else %}
-o buildifier ./buildifier
{% endif %}
{% endblock %}

{% block go_tool %}
bin/go/lang/23
{% endblock %}

{% block install %}
mkdir -p ${out}/bin
cp {{self.go_bins().strip()}} ${out}/bin
{% endblock %}
