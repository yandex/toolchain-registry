{% extends '//die/std/ix.sh' %}

{% block fetch %}
https://devtools-registry.s3.yandex.net/9692301394-clang-20-profiles.tgz
sha:a19e77e01d98338bf63a3ea625f49b1eb59a861d40a5181016860267c90e9964
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
