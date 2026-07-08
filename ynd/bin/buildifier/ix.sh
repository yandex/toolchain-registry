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
f3b800e9f6ca60bdef3709440f393348f7c18a29f30814288a7326285c80aab9
{% endblock %}

{% block go_bins %}
{% if mingw32 %}
buildifier.exe
{% else %}
buildifier
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
