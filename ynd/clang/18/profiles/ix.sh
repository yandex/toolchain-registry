{% extends '//die/std/ix.sh' %}

{% block fetch %}
https://devtools-registry.s3.yandex.net/7406020448-clang-18-profiles.tgz
sha:d7e29e0dbea2f4a439d9626a017c48d242e5e074532d9bcdec22a37f94ac3e59
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
