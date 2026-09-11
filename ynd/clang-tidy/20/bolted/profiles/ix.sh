{% extends '//die/std/ix.sh' %}

{% block fetch %}
http://s3.mds.yandex.net/sandbox-tmp/13531730486/clang-tidy-20-profiles.tgz
sha:da9ec5f619ef30afe2a51f389b8fafd69732eb091c40e27cf405e76b02c6be30
{% endblock %}

{% block unpack %}
mkdir src; cd src; tar -xf ${src}/*tgz
{% endblock %}

{% block install %}
cp ${tmp}/src/bolt-profile.prof ${out}/
{% endblock %}

{% block env %}
export CLANG_TIDY_BOLT_PROFILE=${out}/bolt-profile.prof
{% endblock %}
