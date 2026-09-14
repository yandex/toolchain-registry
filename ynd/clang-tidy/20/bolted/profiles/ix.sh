{% extends '//die/std/ix.sh' %}
{% block fetch %}
https://proxy.sandbox.yandex-team.ru/13624164850
sha:b9a85940a7436da36ab56beded0f3005b55539790ba433beb14f95d7f01b2d3f
{% endblock %}
{% block unpack %}
mkdir src
cd src
for file in "${src}"/*; do
    if [ "${file##*/}" != touch ]; then
        tar -xf "${file}"
    fi
done
{% endblock %}
{% block install %}
cp -a ${tmp}/src/. ${out}/
{% endblock %}
{% block env %}
export CLANG_TIDY_BOLT_PROFILE=${out}/bolt-profile.prof
{% endblock %}
