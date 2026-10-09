{% extends '//die/std/ix.sh' %}

{% block fetch %}
https://devtools-registry.s3.yandex.net/14019461849-clang-22-profiles.tgz
sha:c5ca62b8b5aa93544920cdf5372aa67acab702294967db3973d33bb00ceece07
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
