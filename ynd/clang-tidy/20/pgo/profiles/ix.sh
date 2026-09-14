{% extends '//die/std/ix.sh' %}
{% block fetch %}
https://proxy.sandbox.yandex-team.ru/13623359293
sha:5824db7b5b95557c76405bdbee4691069fbcdad3c41175b2acf9938ffa3cb71b
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
