{% extends '//bld/rust/t/ix.sh' %}

{% block bld_tool %}
bld/rust/96/musl
bin/patch/elf
{% endblock %}

{% block fetch %}
https://static.rust-lang.org/dist/2026-05-28/rust-1.96.0-x86_64-unknown-linux-musl.tar.gz
545aff63f37dea2fcbd8037b877219fca6fbba97660bdcb8d3a0fc5df5fa9edf
https://static.rust-lang.org/dist/2026-05-28/rust-std-1.96.0-aarch64-apple-darwin.tar.gz
a5c160197236f68cc8627a573545fd883d4d98856fb654a6d6aa5883ff1bdcc7
{% endblock %}
