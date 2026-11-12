{% extends '//lib/c++/19/ix.sh' %}

{% block fetch %}
{{super()}}
https://github.com/llvm/llvm-project/commit/e9f851de24baefd5710c3cac6ef3cafaea0eeb9d.diff?full_index=1
sha:ff7d1ab8d00b782061b8d772b07ec2144444e6fe7006868acc5cba9bf4e77b7c
{% endblock %}

{% block unpack %}
# IX's default unpack loop does not recognize .diff files with URL query strings.
mkdir src; cd src
extract1 "${src}"/llvmorg-*.tar.gz
{% endblock %}

{% block patch %}
{{super()}}
patch -p1 < "${src}/e9f851de24baefd5710c3cac6ef3cafaea0eeb9d.diff?full_index=1"
{% endblock %}

{% block install %}
{{super()}}
base64 -d << EOF > ${out}/include/__string/fix.h
{% include '0.diff/base64' %}
EOF
sed -e 's|_LIBCPP_END_NAMESPACE_STD|#include "fix.h"|' \
    -i ${out}/include/__string/char_traits.h
{% endblock %}
