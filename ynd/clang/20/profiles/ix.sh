{% extends '//die/std/ix.sh' %}

{% block fetch %}
https://devtools-registry.s3.yandex.net/XXXXXXXXXX-clang-20-profiles.tgz
sha:XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
{% endblock %}

{% block unpack %}
mkdir src; cd src; tar -xf ${src}/*tgz
{% endblock %}

{% block install %}
cp ${tmp}/src/* ${out}/
{% endblock %}

{% block env %}
export PGO_EXTERNAL_PROFILE=${out}/pgo-profile.prof
export BOLT_EXTERNAL_PROFILE=${out}/bolt-profile.prof
{% endblock %}
