{% extends '//die/std/ix.sh' %}
{% block fetch %}
https://proxy.sandbox.yandex-team.ru/13818078120
sha:df5174291a0d053076811364066acd69065e41d7392758edab2c00740c015e0a
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
