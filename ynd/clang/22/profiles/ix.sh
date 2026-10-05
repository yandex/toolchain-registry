{% extends '//die/std/ix.sh' %}

{% block fetch %}
https://devtools-registry.s3.yandex.net/10491236631-clang-20-profiles.tgz
sha:5fcba5e77e1349169681c8369fbb41a0a86569b30365e679d0171419d3657dca
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
