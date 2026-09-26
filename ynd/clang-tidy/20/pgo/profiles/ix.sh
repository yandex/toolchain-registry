{% extends '//die/std/ix.sh' %}
{% block fetch %}
https://proxy.sandbox.yandex-team.ru/13817144358
sha:739b1fffd9b6855a9dbec521f9ccc9a9b2b584db0c67b22dec85b3dd042b6eef
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
export CLANG_TIDY_PGO_PROFILE=${out}/pgo-profile.prof
{% endblock %}
