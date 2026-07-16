{% extends '//bld/musl/ix.sh' %}

{% block version %}
1.2.5
{% endblock %}

{% block fetch %}
http://musl.libc.org/releases/musl-1.2.5.tar.gz
a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4
{% endblock %}

{% block postinstall %}
{{super()}}
ln -s libgcc_s.so.1 ${out}/lib/libgcc_s.so
{% endblock %}
